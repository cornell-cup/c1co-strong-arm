#include <MovingSteppersLib.h>
#include <MotorEncoderLib.h>
#include <Servo.h>
#include "R2Protocol.h"

#define MAX_ENCODER_VAL 16383

// R2Protocol Definitions
// Jetson to Arduino r2p decode constants
uint8_t data_buffer[32];          // Set equal to 16 + 2 * length of angle array
uint32_t data_buffer_len = 32;    
uint16_t checksum; 
char type[5];
uint8_t data[16];               // Twice the length of the array
uint16_t data_final[8];     
uint32_t data_len;

// Arduino to Jetson R2
uint16_t encoder_angles[] = {0, 0, 0, 0};   

// Create Servo object to control wrist spin
Servo spin_servo;
volatile int spin_pos;
volatile int spin_desired_pos;
volatile bool spin_at_desired; // True means at desired, False means not at desired 

// Create continuous Servo object to control the hand 
Servo hand_servo;

// Stepper motor (elbow) encoder
int s0 = 8;     // step pin
int d0 = 10;    // direction pin
int c0 = 35;    // chip select pin (had this on 5, changing for now)

// Continuous hand encoder pins - step and direction are arbitrary
int s1 = 30;    // step pin - useless
int d1 = 31;    // direction pin - useless
int c1 = 4;     // chip select pin - useful

volatile int fill_serial_buffer = false;
volatile int cont_wait = 0;
volatile int elbow_wait = 0;

// Storing pins and states for the stepper motor (index 0) and continuous Servo (index 1)
MovingSteppersLib motors[2] {{s0,d0,c0}, {s1,d1,c1}};     
int stepPin[2] = {s0, s1}; 
int directionPin[2] = {d0, d1};  
volatile int move [2];              
volatile int state [2];             

int reversed[1] = {0}; // helps elbow choose correct direction 

// Storing encoder values
volatile float encoderDiff[2];    // units of encoder steps
volatile float encoderTarget[2];  // units of encoder steps
volatile float targetAngle[2];    // units of degrees
float encoderPos[2];              // units of encoder steps

// Represent if motors are not at desired positions
volatile int not_tolerant_elbow;         
volatile int not_tolerant_hand;

// New variables for Xbox controller integration
int elbowData = 0;
int spinData = 0;
int handData = 0;
int shoulderData = 0;

int minElbowAngle = 0;
int maxElbowAngle = 120;

int minHandPosition = 0;
int maxHandPosition = 70;

int handAngle = 0;

void setup() {
  Serial.begin(9600);   // Serial monitor
  Serial1.begin(38400); // TX1/RX1 
  reset_input_buffer(); // Jetson to Arduino r2p decode setup (clears Serial monitor)

  // SERVOS
  // Setup for servo that controls wrist spin 
  spin_servo.attach(6);
  spin_desired_pos = 0;
  spin_at_desired = false;

  // Setup for the servo that controls the hand 
  hand_servo.attach(5);
  targetAngle[1] = 1;   // Hand encoder setup: 80 is closed and 1 is open

  // Elbow stepper motor setup
  targetAngle[0] = 0;   // max is roughly 135 if zeroed correctly
  pinMode(directionPin[0], OUTPUT);
  pinMode(stepPin[0], OUTPUT);

  // ENCODERS - first bool for elbow encoder and second bool for hand (true = zero)
  redefine_encoder_zero_position(false, false);

  // ISR TIMER
  // Initialize interrupt timer1 
  noInterrupts();           // disable all interrupts
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 65518;            // preload timer 65536-(16MHz/256/4Hz)

  TCCR1B |= (1 << CS12);    // 256 prescaler 
  TIMSK1 |= (1 << TOIE1);   // enable timer overflow interrupt
  interrupts();             // enable all interrupts
}

ISR(TIMER1_OVF_vect) {        // ISR to pulse pins of moving motors
  TCNT1 = 65518;              // preload timer to 300 us          
  fill_serial_buffer = true;  // check

  // ELBOW STEPPER ISR
  elbow_wait += 1;
  if (elbow_wait == 3) {
    not_tolerant_elbow = abs(encoderDiff[0]) > 10 && ((abs(encoderDiff[0]) + 10) < (MAX_ENCODER_VAL + encoderTarget[0])); // 2nd condition to check if 359 degrees is close enough to 0
    if (move[0]) {                            // if motor should move
      if (not_tolerant_elbow) {               // if not within tolerance
        state[0] = !state[0];                 // toggle state
        digitalWrite(stepPin[0], state[0]);   // write to step pin
      } else {
        move[0] = 0;    // stop moving motor if location reached
      }
    }
    elbow_wait = 0;
  }
  
  // HAND CONTINUOUS ISR
  cont_wait += 1;
  if (cont_wait == 50) { // used to slow down cont Servo movement - prevents Servo library hazards
    not_tolerant_hand = abs(encoderDiff[1]) > 10 && ((abs(encoderDiff[1]) + 10) < (MAX_ENCODER_VAL + encoderTarget[1]));
    if (move[1]) {
      if (not_tolerant_hand) {    // move continuous servo
        cont_check_dir(1);
      } else {
        move[1] = 0;              // stop moving motor if location reached
        hand_servo.write(92);
      }
    }
    cont_wait = 0;
  }
}

void loop() {
  checkDirLongWay(0);

  if (Serial1.available() > 0) {    // Jetson to Arduino
    Serial1.readBytes(data_buffer, data_buffer_len);
    r2p_decode(data_buffer, data_buffer_len, &checksum, type, data, &data_len);

    //Serial.println("Data: ");
    convert_b8_to_b16(data, data_final, 8);         
    for (int i = 0; i < 4; i++) {     
      //Serial.println(data_final[i]);                                 
    }
    controlMovement(data_final);
  }
}

void controlMovement(uint16_t data[]) {
    elbowData = data[0];      // 0 = stop, 1 = in,   2 = out
    spinData = data[1];       // 0 = stop, 1 = CCW,  2 = CW 
    handData = data[2];       // 0 = stop, 1 = open, 2 = close
    shoulderData = data[3];   // 0 = stop, 1 = CCW,  2 = CW

    if (elbowData == 1) {
      targetAngle[0] = maxElbowAngle;
      encoderTarget[0] = targetAngle[0] * 45.51111; // * 360/255;
      encoderPos[0] = motors[0].encoder.getPositionSPI(c0);
      encoderDiff[0] = encoderTarget[0] - encoderPos[0];
      move[0] = 1;                                                    
    } else if (elbowData == 2) {
      targetAngle[0] = minElbowAngle;
      encoderTarget[0] = targetAngle[0] * 45.51111; // * 360/255;
      encoderPos[0] = motors[0].encoder.getPositionSPI(c0);
      encoderDiff[0] = encoderTarget[0] - encoderPos[0];
      move[0] = 1;                                                    
    } else if (elbowData == 0) {
      move[0] = 0;
      encoderPos[0] = motors[0].encoder.getPositionSPI(c0); 
    }

    if (spinData == 1) {
      spin_servo.write(162);
    } else if (spinData == 2) {
      spin_servo.write(22);
    } else if (spinData == 0) {
      spin_servo.write(92);
    }


    handAngle = motors[1].encoder.getPositionSPI(c1)/45.51111;
    Serial.println(handAngle);

    if (handData == 1) {
      hand_servo.write(160);
      // targetAngle[1] = minHandPosition;
      // encoderTarget[1] = targetAngle[1] * 45.51111; // * 360/255;
      // encoderPos[1] = MAX_ENCODER_VAL - motors[1].encoder.getPositionSPI(c1);
      // encoderDiff[1] = encoderTarget[1] - encoderPos[1];
      // move[1] = 1;                                                   
    } else if (handData == 2) {
      hand_servo.write(20);
      // targetAngle[1] = maxHandPosition;
      // encoderTarget[1] = targetAngle[1] * 45.51111; // * 360/255;
      // encoderPos[1] = MAX_ENCODER_VAL - motors[1].encoder.getPositionSPI(c1);
      // encoderDiff[1] = encoderTarget[1] - encoderPos[1];
      // move[1] = 1;                                                    
    } else if (handData == 0) {
        hand_servo.write(92);
        // encoderPos[1] = MAX_ENCODER_VAL - motors[1].encoder.getPositionSPI(c1);
    }
}

// Checks that motor is moving in right direction and switches if not
void checkDirLongWay(int motorNum) { 
  encoderPos[motorNum] = motors[motorNum].encoder.getPositionSPI(c0);
  if (encoderPos[motorNum] == 65535){
    move[motorNum] = 0;   // stop moving if encoder reads error message
  }
  
  if ((MAX_ENCODER_VAL - 1000) < encoderPos[motorNum]) {  // if motor goes past zero incorrectly, we want to make sure it moves back in the correct direction
    encoderPos[motorNum] = 0;
  }
  
  encoderDiff[motorNum] = encoderTarget[motorNum] - encoderPos[motorNum];

  if (encoderDiff[motorNum] > 0) {
    digitalWrite(directionPin[motorNum], !reversed[motorNum]); 
  } else {
    digitalWrite(directionPin[motorNum], reversed[motorNum]);
  }
}

void cont_check_dir(int contNum) {
  // 92/93 - zero rotation
  encoderPos[contNum] = MAX_ENCODER_VAL - motors[contNum].encoder.getPositionSPI(c1);

  if (encoderPos[contNum] == 65535){
    move[contNum] = 0;    // stop moving if encoder reads error message
  }

  if ((MAX_ENCODER_VAL - 1000) < encoderPos[contNum]) {  // if servo goes past zero incorrectly, we want to make sure it moves back in the correct direction
    encoderPos[contNum] = 0;
  }

  encoderDiff[contNum] = encoderTarget[contNum] - encoderPos[contNum];

  if (encoderDiff[contNum] > 0) {
    // Close the hand
    hand_servo.write(10); 
  } else {
    // Open hand
    hand_servo.write(150); 
  }
}

void convert_b8_to_b16(uint8_t *databuffer, uint16_t *data, int len) {
  int data_idx;
  for (int i=0; i < len; i++) {
    data_idx = i / 2;
    if ((i & 1) == 0) {
      // Even
      data[data_idx] = databuffer[i] << 8;
    } else {
      // Odd
      data[data_idx] |= databuffer[i];
    }
  }
}

void reset_input_buffer() {
  while (Serial1.available() > 0) {
    Serial1.read();
    delay(100);
  }
}

// Call this function to reset the encoder's zero position to be the current position of the motor
void redefine_encoder_zero_position(bool rezeroStepper, bool rezeroHand) {
  if (rezeroStepper) {
    motors[0].encoder.setZeroSPI(c0); 
    move[0] = 0;
  }

  if (rezeroHand) {
    motors[1].encoder.setZeroSPI(c1);
    move[1] = 0;
  }
}
