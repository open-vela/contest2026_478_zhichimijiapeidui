/* Mibot ESP32-S3 controller. See README.md before wiring the board. */
#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>

// These are placeholders: replace them with the actual ESP32-S3 carrier pinmap.
static constexpr int I2C_SDA = 8, I2C_SCL = 9;
static constexpr int AIN1 = 4, AIN2 = 5, PWMA = 6;
static constexpr int BIN1 = 7, BIN2 = 15, PWMB = 16, MOTOR_STBY = 17;
static constexpr int SERVO_L = 18, SERVO_R = 21;
static constexpr int SF32_RX = 2, SF32_TX = 1;
static constexpr uint32_t SF32_BAUD = 921600;
static constexpr uint32_t PC_BAUD = 115200;
static constexpr uint8_t TOF_ADDRESS = 0x52; // Verify against the purchased module.
static constexpr uint16_t EDGE_THRESHOLD_MM = 50;
static constexpr uint32_t TOF_TIMEOUT_MS = 250;
static constexpr bool SIMULATE_TOF = false;
static constexpr uint16_t MAX_FRAME_PAYLOAD = 4096;

HardwareSerial Sf32(2);
enum : uint8_t { HELLO=0x01, HELLO_ACK=0x02, COMMAND=0x10, ACK=0x11,
  NACK=0x12, EVENT=0x20, TELEMETRY=0x21, PING=0x60, PONG=0x61 };
enum MotionState : uint8_t { INIT, STANDBY, RUNNING, BRAKING, HOLD, FAULT };
MotionState motion = INIT;
uint16_t sequence = 1;
uint32_t lastRx, lastPing, lastTelemetry, lastTof;
int16_t leftTarget = 0, rightTarget = 0;
uint32_t motionDeadline = 0;
float servoLeft = 90, servoRight = 90;
static char jsonBuffer[MAX_FRAME_PAYLOAD];
static uint8_t rxPayload[MAX_FRAME_PAYLOAD];
static char pcLine[MAX_FRAME_PAYLOAD];
static uint16_t pcLineLen = 0;

uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  while (n--) { c ^= uint16_t(*p++) << 8; for (uint8_t i=0;i<8;i++) c=(c&0x8000)?uint16_t((c<<1)^0x1021):uint16_t(c<<1); }
  return c;
}
uint16_t crc16Append(uint16_t c, const uint8_t *p, size_t n) {
  while (n--) { c ^= uint16_t(*p++) << 8; for (uint8_t i=0;i<8;i++) c=(c&0x8000)?uint16_t((c<<1)^0x1021):uint16_t(c<<1); }
  return c;
}

void sendFrame(uint8_t type, uint8_t flags, const uint8_t *data, uint16_t len) {
  if (len > MAX_FRAME_PAYLOAD) return;
  uint8_t h[7] = {1, type, flags, uint8_t(sequence), uint8_t(sequence>>8), uint8_t(len), uint8_t(len>>8)};
  uint16_t c = crc16Append(crc16(h, sizeof(h)), data, len);
  Sf32.write(0xaa); Sf32.write(0x55); Sf32.write(h, sizeof(h)); if (len) Sf32.write(data, len);
  Sf32.write(uint8_t(c)); Sf32.write(uint8_t(c>>8)); Sf32.flush(); sequence++;
}
void sendJson(uint8_t type, uint8_t flags, JsonDocument &d) {
  size_t n=serializeJson(d,jsonBuffer,sizeof(jsonBuffer)); sendFrame(type,flags,(uint8_t*)jsonBuffer,uint16_t(n));
}
void event(const char *name, const char *severity="error") {
  JsonDocument d; d["schema"]="mibot.event.v1"; d["event"]=name; d["severity"]=severity; d["source"]="esp32.safety"; d["requires_ack"]=false; sendJson(EVENT,0,d);
}

static constexpr uint8_t MOTOR_A_CH=0, MOTOR_B_CH=1, SERVO_L_CH=2, SERVO_R_CH=3;
int16_t speedLimit(int32_t v) { return int16_t(constrain(v,-100,100)); }
void motor(int in1,int in2,uint8_t ch,int16_t speed) {
  speed=speedLimit(speed); if (!speed) { digitalWrite(in1,LOW); digitalWrite(in2,LOW); ledcWrite(ch,0); return; }
  bool f=speed>0; digitalWrite(in1,f); digitalWrite(in2,!f); ledcWrite(ch,map(abs(speed),0,100,0,1023));
}
void stopMotors(bool brake) {
  digitalWrite(AIN1,brake); digitalWrite(AIN2,brake); digitalWrite(BIN1,brake); digitalWrite(BIN2,brake);
  ledcWrite(MOTOR_A_CH,0); ledcWrite(MOTOR_B_CH,0); leftTarget=rightTarget=0;
}
void servo(uint8_t ch,float deg) {
  deg=constrain(deg,0.0f,180.0f); uint32_t us=500+uint32_t(2000*deg/180.0f);
  ledcWrite(ch,(us*65535UL)/20000UL);
}

struct Reading { uint16_t mm=0; uint8_t quality=0; bool valid=false; uint32_t at=0; } tof[4];
// TOF050C/200C/400C register transactions vary by vendor. Fill this from the module datasheet.
bool readTof(uint8_t index,uint16_t &mm,uint8_t &quality) {
  (void)index; if (SIMULATE_TOF) { mm=300; quality=90; return true; }
  (void)mm; (void)quality; return false;
}
bool tofOk() { for (auto &r:tof) if (!r.valid || millis()-r.at>TOF_TIMEOUT_MS) return false; return true; }
bool edge() { return (tof[0].valid && tof[0].mm>EDGE_THRESHOLD_MM) || (tof[1].valid && tof[1].mm>EDGE_THRESHOLD_MM); }
void updateTof() {
  uint32_t now=millis(); if (now-lastTof<50) return; lastTof=now;
  for (uint8_t i=0;i<4;i++) { uint16_t mm=0; uint8_t q=0; bool ok=readTof(i,mm,q); tof[i]={mm,q,ok&&q>0,now}; }
}
void brake(const char *why) { stopMotors(true); motion=BRAKING; event(why,"critical"); }
void updateSafety() {
  updateTof(); uint32_t now=millis();
  if (motion==RUNNING && !tofOk()) brake("tof_invalid");
  if (motion==RUNNING && (leftTarget>0 || rightTarget>0) && edge()) brake("edge_detected");
  if (motion==RUNNING && now>=motionDeadline) { stopMotors(false); motion=STANDBY; }
  if (motion==RUNNING && now-lastRx>1500) brake("uart_timeout");
  if (motion==BRAKING && !edge()) motion=HOLD;
}

void reply(const char *id,bool ok,const char *code=nullptr) {
  JsonDocument d; d["schema"]="mibot.uart.v1"; d["ok"]=ok; if(id)d["command_id"]=id; d["state"]=ok?"accepted":"rejected";
  if(!ok){JsonObject e=d["error"].to<JsonObject>();e["code"]=code?code:"E_INVALID_ARG";} sendJson(ok?ACK:NACK,ok?2:6,d);
}
void command(const uint8_t *p,uint16_t n) {
  JsonDocument d; if(deserializeJson(d,p,n)){reply(nullptr,false);return;} JsonObjectConst r=d.as<JsonObjectConst>();
  const char *name=r["name"]|"";
  const char *id=r["command_id"].is<const char *>() ? r["command_id"].as<const char *>() : nullptr;
  JsonObjectConst a=r["args"].as<JsonObjectConst>();
  if(!strcmp(name,"robot.stop")){bool e=a["emergency"]|false;stopMotors(e);motion=e?BRAKING:STANDBY;reply(id,true);return;}
  if(!strcmp(name,"robot.move")){
    int32_t linear=a["linear_mm_s"]|0, angular=a["angular_deg_s"]|0; uint32_t dur=constrain(a["duration_ms"]|500UL,50UL,5000UL);
    if(linear<-120||linear>120||angular<-90||angular>90){reply(id,false,"E_INVALID_ARG");return;}
    bool forward=linear>0||(linear==0&&angular!=0);
    const bool sensorsValid = tofOk();
    const bool blockedByEdge = forward && edge();
    if(motion==FAULT || motion==BRAKING || !sensorsValid || blockedByEdge){
      reply(id,false,sensorsValid?"E_SAFETY_LOCK":"E_TOF_INVALID");
      return;
    }
    leftTarget=speedLimit(linear+angular); rightTarget=speedLimit(linear-angular); motor(AIN1,AIN2,MOTOR_A_CH,leftTarget); motor(BIN1,BIN2,MOTOR_B_CH,rightTarget);
    digitalWrite(MOTOR_STBY,HIGH); motionDeadline=millis()+dur; motion=RUNNING; reply(id,true); return;
  }
  if(!strcmp(name,"robot.set_arm_pose")){
    float l=a["left_deg"]|servoLeft, rr=a["right_deg"]|servoRight; if(l<0||l>180||rr<0||rr>180){reply(id,false,"E_SERVO_LIMIT");return;}
    servoLeft=l;servoRight=rr;servo(SERVO_L_CH,l);servo(SERVO_R_CH,rr);reply(id,true);return;
  }
  if(!strcmp(name,"robot.get_status")||!strcmp(name,"robot.read_floor_sensors")){reply(id,true);return;}
  reply(id,false,"E_UNSUPPORTED");
}

// The USB UART is a simple newline-delimited JSON console for PC validation.
// Valid JSON commands are forwarded unchanged as COMMAND frames to SF32.
void pcPoll() {
  while (Serial.available()) {
    char c = char(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      pcLine[pcLineLen] = '\0';
      if (pcLineLen) {
        JsonDocument d;
        DeserializationError err = deserializeJson(d, pcLine, pcLineLen);
        if (err) {
          Serial.printf("PC_ERR invalid_json %s\n", err.c_str());
        } else {
          sendJson(COMMAND, 0, d);
          Serial.printf("PC_TX %u bytes\n", unsigned(pcLineLen));
        }
      }
      pcLineLen = 0;
    } else if (pcLineLen + 1 < MAX_FRAME_PAYLOAD) {
      pcLine[pcLineLen++] = c;
    } else {
      pcLineLen = 0;
      Serial.println("PC_ERR line_too_long");
    }
  }
}

void framePoll() {
  static uint8_t s=0,h[7],cb[2]; static uint16_t len=0,i=0;
  while(Sf32.available()) { uint8_t b=Sf32.read();
    if(s==0){if(b==0xaa)s=1;continue;} if(s==1){s=(b==0x55)?2:0;continue;}
    if(s>=2&&s<=8){h[s-2]=b;if(s==8){len=h[5]|uint16_t(h[6])<<8;if(h[0]!=1||len>MAX_FRAME_PAYLOAD){s=0;continue;}i=0;s=len?9:10;}else s++;continue;}
    if(s==9){rxPayload[i++]=b;if(i>=len)s=10;continue;} if(s==10){cb[0]=b;s=11;continue;} cb[1]=b;
    uint16_t got=cb[0]|uint16_t(cb[1])<<8;
    if(crc16Append(crc16(h,7),rxPayload,len)==got){
      lastRx=millis();
      if(h[1]==COMMAND) command(rxPayload,len);
      else if(h[1]==PING) sendFrame(PONG,2,rxPayload,len);
      else if(h[1]==HELLO){JsonDocument d;d["device"]="esp32s3";d["firmware"]="mibot-esp32-0.1.0";d["protocol_version"]=1;sendJson(HELLO_ACK,2,d);}
      else if(h[1]==ACK || h[1]==NACK || h[1]==EVENT || h[1]==TELEMETRY){
        Serial.write(rxPayload, len); Serial.write('\n');
      }
    } else event("protocol_crc_error"); s=0;
  }
}
void telemetry() {
  uint32_t now=millis(), period=motion==RUNNING?100:1000;if(now-lastTelemetry<period)return;lastTelemetry=now;JsonDocument d;
  d["schema"]="mibot.telemetry.v1";d["ts_ms"]=now;d["motion_state"]=uint8_t(motion);d["battery_mv"]=0;JsonArray x=d["tof"].to<JsonArray>();for(auto&r:tof){JsonObject q=x.add<JsonObject>();q["mm"]=r.mm;q["valid"]=r.valid;q["quality"]=r.quality;}sendJson(TELEMETRY,0,d);
}
void setup() {
  Serial.begin(PC_BAUD);Sf32.begin(SF32_BAUD,SERIAL_8N1,SF32_RX,SF32_TX);Wire.begin(I2C_SDA,I2C_SCL,400000);
  for(int p:{AIN1,AIN2,BIN1,BIN2,MOTOR_STBY})pinMode(p,OUTPUT);digitalWrite(MOTOR_STBY,LOW);
  ledcSetup(MOTOR_A_CH,20000,10);ledcSetup(MOTOR_B_CH,20000,10);ledcAttachPin(PWMA,MOTOR_A_CH);ledcAttachPin(PWMB,MOTOR_B_CH);stopMotors(false);
  ledcSetup(SERVO_L_CH,50,16);ledcSetup(SERVO_R_CH,50,16);ledcAttachPin(SERVO_L,SERVO_L_CH);ledcAttachPin(SERVO_R,SERVO_R_CH);servo(SERVO_L_CH,servoLeft);servo(SERVO_R_CH,servoRight);
  motion=STANDBY;lastRx=millis();JsonDocument d;d["device"]="esp32s3";d["firmware"]="mibot-esp32-0.1.0";sendJson(HELLO_ACK,2,d);
}
void loop(){pcPoll();framePoll();updateSafety();if(millis()-lastPing>=500){lastPing=millis();JsonDocument d;d["ts_ms"]=millis();sendJson(PING,1,d);}telemetry();delay(1);}
