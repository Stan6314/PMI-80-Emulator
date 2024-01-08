/* A PMI-80 single-board computer emulator based on the FabGL library
 * -> see http://www.fabglib.org/ or FabGL on GitHub.
 *  
 * For proper operation, an ESP32 module with a VGA monitor 
 * and a PS2 keyboard or mouse connected according to the 
 * diagram on the website www.fabglib.org is required.
 * 
 * The PMI80 emulator is slightly improved (hence PMI80A).
 * RAM is expanded to 7 KB and "cassete recorder" is included.
 * Cassette recorder is emulated using SPIFFS (max 256 blocks).
 * Optionally, an MCP23S17 expander can be used to emulate a 
 * secondary 8255 (ports PA and PB only) - connected: 
 * MISO = 35, MOSI = 12, CLK = 14, CS = 13(inverted)
 * 
 * PMI80A is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or any later version.
 * PMI80A is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
 * Stan Pechal, 2022
*/
#include "fabgl.h"
#include "fabui.h"
#include "SPIFFS.h"                 // For "cassette recorder"
#include "devdrivers/MCP23S17.h"    // For "extended 8255"
#include "emudevs/i8080.h"          // For processor

// Hardware used on the ESP32 board: VGA display + PS mouse and keyboard
fabgl::VGA16Controller DisplayController;
fabgl::PS2Controller PS2Controller;

// Hardware emulated on the PMI80A computer
// Processor I8080 will be used from the library FabGL
fabgl::i8080 m_i8080;
// ROM memory is contained in the file "pmi80rom.h"
#include "pmi80rom.h"
// RAM memory will be just Byte array - originally it was 1 KB, but we can use bigger memory
unsigned char pmi80ram[8192];   // the first KB is overwritten by ROM
// Variables for emulating primary 8255 (display and keyboard connection)
int nCathode,portPC;    // Active cathode, data sent to PC port
int keyboardIn[9];      // Value read from keyboard PC port
// Callback functions defined below for transferring data over the bus
static int readByte(void * context, int address);
static void writeByte(void * context, int address, int value);
static int readWord(void * context, int addr);
static void writeWord(void * context, int addr, int value);
static int readIO(void * context, int address);
static void writeIO(void * context, int address, int value);
// Flag for "cassette recorder" is ready
bool readySPIFFS = false;
// MCP23S17 can emulate secondary 8255 on the PMI80 board (if present)
fabgl::MCP23S17  mcp2317;
bool readyMCP2317 = false;

// Most of the activity happens in the App
struct TestApp : public uiApp {

  uiFrame * dispFrame, * frame;       // Frames for program and for display
  uiButton *       keyButton[25];     // Software buttons for PMI80
  uiTimerHandle    procTimer;         // Timer for processor timing
  uiButton *       startButton;       // Start/Stop button
  volatile bool    runPMI80 = false;  // Flag for run or stop processor
  uiLabel * freeMemLabel;             // Labels with free memory, "8255" present and static texts
  uiStaticLabel * progName, * startKey, * keybHelp, * extended8255Status;
  uiColorBox * segment[9][7];         // Colorboxes for 9 digits of seven segment display
  int lastChar[9];                    // Last displayed character (repainted only if it is different)
  int timeOutChar[9];                 // Time-out for switch of digit (afterglow)

  // Define colors for graphic UI
  #define SEGMENTON (Color)9
  #define SEGMENTOFF (Color)7
  #define BACKCOLOR (Color)8

  // Show free memory in SPIFFS
  void showFreeMemory()
  {
    freeMemLabel->setTextFmt("\"Cassette recorder\" free memory: %d KB", (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024);
    freeMemLabel->repaint();
  }

  // Show character (value) on the position (cathode) of 7segments display
  void setSegments(int cathode, int value) 
  {
    if((value & 0x7F) == 0x7F) return;    // Clearing digit on this position will be done automatically by timer
    timeOutChar[cathode] = 50;            // For visible character set new Time-out
    if(lastChar[cathode] != value) {      // But display it only if is different from last character
      lastChar[cathode] = value;          // Save it for next assessment whether to display
      if(value & 0x01) segment[cathode][0]->setColor(SEGMENTOFF); else segment[cathode][0]->setColor(SEGMENTON);
      if(value & 0x02) segment[cathode][1]->setColor(SEGMENTOFF); else segment[cathode][1]->setColor(SEGMENTON);
      if(value & 0x04) segment[cathode][2]->setColor(SEGMENTOFF); else segment[cathode][2]->setColor(SEGMENTON);
      if(value & 0x08) segment[cathode][3]->setColor(SEGMENTOFF); else segment[cathode][3]->setColor(SEGMENTON);
      if(value & 0x10) segment[cathode][4]->setColor(SEGMENTOFF); else segment[cathode][4]->setColor(SEGMENTON);
      if(value & 0x20) segment[cathode][5]->setColor(SEGMENTOFF); else segment[cathode][5]->setColor(SEGMENTON);
      if(value & 0x40) segment[cathode][6]->setColor(SEGMENTOFF); else segment[cathode][6]->setColor(SEGMENTON);
    }
  }

  // Clear display position - digit on "cathode" position
  void clearSegments(int cathode) 
  {
    for (int i = 0; i < 7; ++i) segment[cathode][i]->setColor(SEGMENTOFF);
    lastChar[cathode] = 0x7F;
  }

  // Most of functions is started in init() application 
  void init() {
    // Frames and auxiliary texts
    rootWindow()->frameStyle().backgroundColor = BACKCOLOR;
    frame = rootWindow();
    // some static text
    progName = new uiStaticLabel(frame, "PMI-80 (see https://sk.wikipedia.org/wiki/PMI-80) Emulator based on www.fabglib.org - SP 2022", Point(20, 20));
    progName->labelStyle().backgroundColor = frame->frameStyle().backgroundColor;
    progName->repaint();
    startKey = new uiStaticLabel(frame, "Run/Stop Emulator", Point(55, 230));
    startKey->labelStyle().backgroundColor = frame->frameStyle().backgroundColor;
    startKey->repaint();
    keybHelp = new uiStaticLabel(frame, "Use keyboard (Keys in brackets) or mouse", Point(290, 230));
    keybHelp->labelStyle().backgroundColor = frame->frameStyle().backgroundColor;
    keybHelp->repaint();
    // frame where to put display segments
    dispFrame = new uiFrame(rootWindow(), "", Point(20, 50), Size(600, 140));
    dispFrame->frameStyle().backgroundColor = SEGMENTOFF;
    dispFrame->windowStyle().borderSize     = 0;

    // Dimensions for display segments and spaces between
    #define DISPXOFS 30
    #define DISPYOFS 30
    #define SEGLEN 30
    #define SEGWIDTH 5
    #define SEGCORN 2
    #define SEGSPACE 18
    // Placement of individual segments in their positions
    for (int i = 0; i < 9; ++i) {
      segment[i][0] = new uiColorBox(dispFrame, Point(DISPXOFS+SEGWIDTH+SEGCORN+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS), Size(SEGLEN, SEGWIDTH), BACKCOLOR);  // Segment A
      segment[i][1] = new uiColorBox(dispFrame, Point(DISPXOFS+SEGWIDTH+2*SEGCORN+SEGLEN+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+SEGWIDTH+SEGCORN), Size(SEGWIDTH, SEGLEN), BACKCOLOR);  // Segment B
      segment[i][2] = new uiColorBox(dispFrame, Point(DISPXOFS+SEGWIDTH+2*SEGCORN+SEGLEN+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+SEGLEN+2*SEGWIDTH+3*SEGCORN), Size(SEGWIDTH, SEGLEN), BACKCOLOR);  // Segment C
      segment[i][3] = new uiColorBox(dispFrame, Point(DISPXOFS+SEGWIDTH+SEGCORN+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+2*SEGLEN+2*SEGWIDTH+4*SEGCORN), Size(SEGLEN, SEGWIDTH), BACKCOLOR);  // Segment D
      segment[i][4] = new uiColorBox(dispFrame, Point(DISPXOFS+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+SEGLEN+2*SEGWIDTH+3*SEGCORN), Size(SEGWIDTH, SEGLEN), BACKCOLOR);  // Segment E
      segment[i][5] = new uiColorBox(dispFrame, Point(DISPXOFS+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+SEGWIDTH+SEGCORN), Size(SEGWIDTH, SEGLEN), BACKCOLOR);  // Segment F
      segment[i][6] = new uiColorBox(dispFrame, Point(DISPXOFS+SEGWIDTH+SEGCORN+i*(SEGLEN+2*SEGWIDTH+2*SEGCORN+SEGSPACE), DISPYOFS+SEGLEN+SEGWIDTH+2*SEGCORN), Size(SEGLEN, SEGWIDTH), BACKCOLOR);  // Segment G
    }
    for (int i = 0; i < 9; ++i) for (int j = 0; j < 7; ++j) segment[i][j]->setFocusIndex(-1);   // Turn off tabulator focus for segments
    // Clear variables for display interface and keyboard
    for (int i = 0; i < 9; ++i) lastChar[i]=0x7F;
    for (int i = 0; i < 9; ++i) keyboardIn[i]=0x7F;

    // Start and Stop button - it handles runPMI flag only
    startButton = new uiButton(frame, "Start(T)", Point(60, 250), Size(80, 20), uiButtonKind::Switch);
    startButton->setFocusIndex(-1);
    startButton->onChange = [&]() {
      if (startButton->down()) {
        startButton->setText("Stop(T)");
        runPMI80 = true;
      } else {
        startButton->setText("Start(T)");
        runPMI80 = false;
      }
    };

    // Dimensions for software (graphic) keyboard
    #define KEYXOFS 250
    #define KEYYOFS 250
    #define KEYXSIZE 50
    #define KEYYSIZE 20
    #define KEYXSPACE 10
    #define KEYYSPACE 10
    // Placement 25 keys on the main frame and settings their functions
    keyButton[0] = new uiButton(frame, "RE(z)", Point(KEYXOFS, KEYYOFS), Size(KEYXSIZE, KEYYSIZE));
    keyButton[0]->onClick = [&]() { m_i8080.reset(); };
    keyButton[1] = new uiButton(frame, "I(i)", Point(KEYXOFS+KEYXSIZE+KEYXSPACE, KEYYOFS), Size(KEYXSIZE, KEYYSIZE));
    keyButton[1]->onClick = [&]() { m_i8080.interruptRST(7); };    // omit cpu cycles
    keyButton[2] = new uiButton(frame, "EX(x)", Point(KEYXOFS+2*KEYXSIZE+2*KEYXSPACE, KEYYOFS), Size(KEYXSIZE, KEYYSIZE));
    keyButton[2]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[3] = 0x3F; };
    keyButton[2]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[3] = 0x7F; };
    keyButton[3] = new uiButton(frame, "R(r)", Point(KEYXOFS+3*KEYXSIZE+3*KEYXSPACE, KEYYOFS), Size(KEYXSIZE, KEYYSIZE));
    keyButton[3]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[3] = 0x5F; };
    keyButton[3]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[3] = 0x7F; };
    keyButton[4] = new uiButton(frame, "BR(q)", Point(KEYXOFS+4*KEYXSIZE+4*KEYXSPACE, KEYYOFS), Size(KEYXSIZE, KEYYSIZE));
    keyButton[4]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x6F; };
    keyButton[4]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x7F; };
    keyButton[5] = new uiButton(frame, "C", Point(KEYXOFS, KEYYOFS+KEYYSIZE+KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[5]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x3F; };
    keyButton[5]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x7F; };
    keyButton[6] = new uiButton(frame, "D", Point(KEYXOFS+KEYXSIZE+KEYXSPACE, KEYYOFS+KEYYSIZE+KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[6]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x3F; };
    keyButton[6]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x7F; };
    keyButton[7] = new uiButton(frame, "E", Point(KEYXOFS+2*KEYXSIZE+2*KEYXSPACE, KEYYOFS+KEYYSIZE+KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[7]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x5F; };
    keyButton[7]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x7F; };
    keyButton[8] = new uiButton(frame, "F", Point(KEYXOFS+3*KEYXSIZE+3*KEYXSPACE, KEYYOFS+KEYYSIZE+KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[8]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x5F; };
    keyButton[8]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[4] = 0x7F; };
    keyButton[9] = new uiButton(frame, "M(m)", Point(KEYXOFS+4*KEYXSIZE+4*KEYXSPACE, KEYYOFS+KEYYSIZE+KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[9]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x6F; };
    keyButton[9]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[5] = 0x7F; };
    keyButton[10] = new uiButton(frame, "8", Point(KEYXOFS, KEYYOFS+2*KEYYSIZE+2*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[10]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x3F; };
    keyButton[10]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x7F; };
    keyButton[11] = new uiButton(frame, "9", Point(KEYXOFS+KEYXSIZE+KEYXSPACE, KEYYOFS+2*KEYYSIZE+2*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[11]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[6] = 0x3F; };
    keyButton[11]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[6] = 0x7F; };
    keyButton[12] = new uiButton(frame, "A", Point(KEYXOFS+2*KEYXSIZE+2*KEYXSPACE, KEYYOFS+2*KEYYSIZE+2*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[12]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x5F; };
    keyButton[12]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x7F; };
    keyButton[13] = new uiButton(frame, "B", Point(KEYXOFS+3*KEYXSIZE+3*KEYXSPACE, KEYYOFS+2*KEYYSIZE+2*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[13]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[6] = 0x5F; };
    keyButton[13]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[6] = 0x7F; };
    keyButton[14] = new uiButton(frame, "L(l)", Point(KEYXOFS+4*KEYXSIZE+4*KEYXSPACE, KEYYOFS+2*KEYYSIZE+2*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[14]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x6F; };
    keyButton[14]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[2] = 0x7F; };
    keyButton[15] = new uiButton(frame, "4", Point(KEYXOFS, KEYYOFS+3*KEYYSIZE+3*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[15]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x3F; };
    keyButton[15]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x7F; };
    keyButton[16] = new uiButton(frame, "5", Point(KEYXOFS+KEYXSIZE+KEYXSPACE, KEYYOFS+3*KEYYSIZE+3*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[16]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[7] = 0x3F; };
    keyButton[16]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[7] = 0x7F; };
    keyButton[17] = new uiButton(frame, "6", Point(KEYXOFS+2*KEYXSIZE+2*KEYXSPACE, KEYYOFS+3*KEYYSIZE+3*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[17]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x5F; };
    keyButton[17]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x7F; };
    keyButton[18] = new uiButton(frame, "7", Point(KEYXOFS+3*KEYXSIZE+3*KEYXSPACE, KEYYOFS+3*KEYYSIZE+3*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[18]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[7] = 0x5F; };
    keyButton[18]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[7] = 0x7F; };
    keyButton[19] = new uiButton(frame, "S(s)", Point(KEYXOFS+4*KEYXSIZE+4*KEYXSPACE, KEYYOFS+3*KEYYSIZE+3*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[19]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x6F; };
    keyButton[19]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[1] = 0x7F; };
    keyButton[20] = new uiButton(frame, "0", Point(KEYXOFS, KEYYOFS+4*KEYYSIZE+4*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[20]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[0] = 0x3F; };
    keyButton[20]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[0] = 0x7F; };
    keyButton[21] = new uiButton(frame, "1", Point(KEYXOFS+KEYXSIZE+KEYXSPACE, KEYYOFS+4*KEYYSIZE+4*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[21]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x3F; };
    keyButton[21]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x7F; };
    keyButton[22] = new uiButton(frame, "2", Point(KEYXOFS+2*KEYXSIZE+2*KEYXSPACE, KEYYOFS+4*KEYYSIZE+4*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[22]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[0] = 0x5F; };
    keyButton[22]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[0] = 0x7F; };
    keyButton[23] = new uiButton(frame, "3", Point(KEYXOFS+3*KEYXSIZE+3*KEYXSPACE, KEYYOFS+4*KEYYSIZE+4*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[23]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x5F; };
    keyButton[23]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x7F; };
    keyButton[24] = new uiButton(frame, "=(Ent)", Point(KEYXOFS+4*KEYXSIZE+4*KEYXSPACE, KEYYOFS+4*KEYYSIZE+4*KEYYSPACE), Size(KEYXSIZE, KEYYSIZE));
    keyButton[24]->onMouseDown = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x6F; };
    keyButton[24]->onMouseUp = [&](const fabgl::uiMouseEventInfo myAuxVar) { keyboardIn[8] = 0x7F; };
    for (int i = 0; i < 25; ++i) keyButton[i]->setFocusIndex(-1);

    // Keyboard interface for selected keys
    // Handles Key Up following keys:
    frame->onKeyUp = [&](uiKeyEventInfo const & key) {
      switch (key.VK) {
      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_0:  // 0
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_2: keyboardIn[0] = 0x7F; break;  // 2
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_4:  // 4
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_6:  // 6
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] = 0x7F; break;  // s-S
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_8:  // 8
      case VirtualKey::VK_a:
      case VirtualKey::VK_A:  // a-A
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[2] = 0x7F; break;  // l-L
      case VirtualKey::VK_r:
      case VirtualKey::VK_R:  // r-R
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[3] = 0x7F; break;  // x-X
      case VirtualKey::VK_d:
      case VirtualKey::VK_D:  // d-D
      case VirtualKey::VK_f:
      case VirtualKey::VK_F:  // f-F
      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[4] = 0x7F; break;  // q-Q
      case VirtualKey::VK_c:
      case VirtualKey::VK_C:  // c-C
      case VirtualKey::VK_e:
      case VirtualKey::VK_E:  // e-E
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[5] = 0x7F; break;  // m-M
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_9:  // 9
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[6] = 0x7F; break;  // b-B
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_5:  // 5
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_7: keyboardIn[7] = 0x7F; break;  // 7
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_1:  // 1
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_3:  // 3
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[8] = 0x7F; break;  // = Enter
      default: break;
      }
    };

    // Handles Key Down following keys:
    frame->onKeyDown = [&](uiKeyEventInfo const & key) {
      switch (key.VK) {
      case VirtualKey::VK_KP_0:
      case VirtualKey::VK_0: keyboardIn[0] = 0x3F; break;  // 0
      case VirtualKey::VK_KP_1:
      case VirtualKey::VK_1: keyboardIn[8] = 0x3F; break;  // 1
      case VirtualKey::VK_KP_2:
      case VirtualKey::VK_2: keyboardIn[0] = 0x5F; break;  // 2
      case VirtualKey::VK_KP_3:
      case VirtualKey::VK_3: keyboardIn[8] = 0x5F; break;  // 3
      case VirtualKey::VK_KP_4:
      case VirtualKey::VK_4: keyboardIn[1] = 0x3F; break;  // 4
      case VirtualKey::VK_KP_5:
      case VirtualKey::VK_5: keyboardIn[7] = 0x3F; break;  // 5
      case VirtualKey::VK_KP_6:
      case VirtualKey::VK_6: keyboardIn[1] = 0x5F; break;  // 6
      case VirtualKey::VK_KP_7:
      case VirtualKey::VK_7: keyboardIn[7] = 0x5F; break;  // 7
      case VirtualKey::VK_KP_8:
      case VirtualKey::VK_8: keyboardIn[2] = 0x3F; break;  // 8
      case VirtualKey::VK_KP_9:
      case VirtualKey::VK_9: keyboardIn[6] = 0x3F; break;  // 9
      case VirtualKey::VK_a:
      case VirtualKey::VK_A: keyboardIn[2] = 0x5F; break;  // a-A
      case VirtualKey::VK_b:
      case VirtualKey::VK_B: keyboardIn[6] = 0x5F; break;  // b-B
      case VirtualKey::VK_c:
      case VirtualKey::VK_C: keyboardIn[5] = 0x3F; break;  // c-C
      case VirtualKey::VK_d:
      case VirtualKey::VK_D: keyboardIn[4] = 0x3F; break;  // d-D
      case VirtualKey::VK_e:
      case VirtualKey::VK_E: keyboardIn[5] = 0x5F; break;  // e-E
      case VirtualKey::VK_f:
      case VirtualKey::VK_F: keyboardIn[4] = 0x5F; break;  // f-F
      case VirtualKey::VK_l:
      case VirtualKey::VK_L: keyboardIn[2] = 0x6F; break;  // l-L
      case VirtualKey::VK_m:
      case VirtualKey::VK_M: keyboardIn[5] = 0x6F; break;  // m-M
      case VirtualKey::VK_q:
      case VirtualKey::VK_Q: keyboardIn[4] = 0x6F; break;  // q-Q
      case VirtualKey::VK_r:
      case VirtualKey::VK_R: keyboardIn[3] = 0x5F; break;  // r-R
      case VirtualKey::VK_s:
      case VirtualKey::VK_S: keyboardIn[1] = 0x6F; break;  // s-S
      case VirtualKey::VK_x:
      case VirtualKey::VK_X: keyboardIn[3] = 0x3F; break;  // x-X
      case VirtualKey::VK_EQUALS:
      case VirtualKey::VK_RETURN:
      case VirtualKey::VK_KP_ENTER: keyboardIn[8] = 0x6F; break;  // = Enter
      case VirtualKey::VK_i:
      case VirtualKey::VK_I: m_i8080.interruptRST(7); break;  // i-I
      case VirtualKey::VK_z:
      case VirtualKey::VK_Z: m_i8080.reset(); break;  // z-Z
      case VirtualKey::VK_t:
      case VirtualKey::VK_T: if(runPMI80) { 
                                startButton->setDown(false); 
                                startButton->setText("Start(T)");
                                runPMI80 = false;
                            } else { 
                                startButton->setDown(true); 
                                startButton->setText("Stop(T)");
                                runPMI80 = true;
                            } break;  // t-T
      default: break;
      }
    };

    // Label to show free memory in "Cassette recorder"
    freeMemLabel = new uiLabel(frame, "", Point(20, 440));
    freeMemLabel->labelStyle().backgroundColor = frame->frameStyle().backgroundColor;
    showFreeMemory();

    // 1 milisecond timer microcomputer control run
    procTimer = setTimer(frame, 1);
    frame->onTimer = [&](uiTimerHandle h) {
      // timer for 1 milisec procesor run
      if (h == procTimer) {
        int cpuCycles = 0;
        if(runPMI80) {
          while(cpuCycles < 1100) cpuCycles += m_i8080.step(); // run CPU
          for (int i = 0; i < 9; ++i) {   // and check afterglow of display digits
            if(timeOutChar[i]) {
              timeOutChar[i]-=1;
              if(!timeOutChar[i]) clearSegments(i);
            }
          }
        }
      }
    };

    // Test and display if MCP23S17 is available
    extended8255Status = new uiStaticLabel(frame, "", Point(20, 420));
    extended8255Status->labelStyle().backgroundColor = frame->frameStyle().backgroundColor;
    if(mcp2317.begin()) {
      extended8255Status->setText("MCP23S17 available - Extended 8255 PA and PB Ports ready");
      extended8255Status->repaint();
      readyMCP2317 = true;
    } else  {
      extended8255Status->setText("MCP23S17 not available - No Extended 8255 Ports");
      extended8255Status->repaint();
    };

    // Set CPU bus functions and start it
    m_i8080.setCallbacks(this, readByte, writeByte, readWord, writeWord, readIO, writeIO); 
    m_i8080.reset();
  }
} app;


// Functions for communication on the bus
static int readByte(void * context, int address)              { if(address < 0x400) return(pmi80rom[address]); else return(pmi80ram[address & 0x1FFF]); };
static void writeByte(void * context, int address, int value) { pmi80ram[address & 0x1FFF] = (unsigned char)value; };
static int readWord(void * context, int addr)                 { return readByte(context, addr) | (readByte(context, addr + 1) << 8); };
static void writeWord(void * context, int addr, int value)    { writeByte(context, addr, value & 0xFF); writeByte(context, addr + 1, value >> 8); } ;
static int readIO(void * context, int address)
{
  switch (address) {
    // *** 8255 primary port - only PC for keyboard
    case 0xFA:      // PC port
      return keyboardIn[nCathode];
    break;
    // *** 8255 secondary ports - only PA and PB ports in Mode 0
    case 0xF4:      // PA port
      if(readyMCP2317) return mcp2317.readPort(MCP_PORTA); else return 0xFF;
    break;
    case 0xF5:      // PB port
      if(readyMCP2317) return mcp2317.readPort(MCP_PORTB); else return 0xFF;
    break;
    default: return 0xFF; break;  // Return "not selected I/O" - bus with 0xFF
  }
};
static void writeIO(void * context, int address, int value)
{
  // ****  Part with 2 special addresses for emulation "cassette recorder" **** 
  // Writing to these addresses will "start recorder" replaced by SPIFFS
  switch (address) {
    case 0x5A:
        { // Save function
          unsigned int auxAdr=(pmi80ram[0x1FF9]<<8) | pmi80ram[0x1FF8];
          if(auxAdr < 0x2000) {
          String namefile = "/pmi_"; namefile+=pmi80ram[0x1FFA]; namefile+=".bin";
          if(readySPIFFS) {
            File file = SPIFFS.open(namefile, "wb");
            if(file){
              if(file.write(pmi80ram + auxAdr, (((~pmi80ram[0x1FF8])&0xFF)+1))) {
                app.runPMI80=false;
                app.app()->messageBox("Saved to:", namefile.c_str(), "OK",  nullptr, nullptr, uiMessageBoxIcon::Info);
                app.showFreeMemory();
                app.runPMI80=true;
                m_i8080.setPC(0x386); // MG Stop
              } else m_i8080.setPC(0x197);  // Error address in save function
            file.close();
            } else m_i8080.setPC(0x197);
          } else m_i8080.setPC(0x197);
        }
    }
    break;
    case 0xAD:
        { // Load function
          // Build file name at first 
          String namefile = "/pmi_"; namefile+=pmi80ram[0x1FFA]; namefile+=".bin";
          if(readySPIFFS) {
            // If SPIFFS is ready, try open file
            File file = SPIFFS.open(namefile, "rb");
            if(file){ // Error will exit file load
            size_t fileSize = file.size();            // Get size of file
            unsigned int auxAdr=(pmi80ram[0x1FF9]<<8) | pmi80ram[0x1FF8];   // Start address in memory
            if((auxAdr + fileSize) < 0x2000) {            // Test if not crossed the end of the memory
              fileSize = file.readBytes((char*)(pmi80ram + auxAdr),file.size());   // Read file to memory
              app.runPMI80=false; // Message
              String messageload = "Loaded "; messageload+=fileSize; messageload+=" bytes";
              app.app()->messageBox("SPIFFS:", messageload.c_str(), "OK",  nullptr, nullptr, uiMessageBoxIcon::Info);
              app.runPMI80=true;
              m_i8080.setPC(0x386); // MG Stop
            } else  m_i8080.setPC(0x197);
            file.close();
            } else  m_i8080.setPC(0x197);
          }
    }
    break;
    // ****************  End of "cassette recorder" **************
    // *** 8255 primary ports - keyboard and display connection
    case 0xF8:      // PA port
      if(nCathode < 9) {
        app.setSegments(nCathode,value);
    }
    break;
    case 0xFA:      // PC port
      portPC = value; nCathode=(~value)&0x0F;
    break;
    // *** 8255 secondary ports - only PA and PB ports in Mode 0
    case 0xF4:      // PA port
      if(readyMCP2317) mcp2317.writePort(MCP_PORTA, (uint8_t) value);
    break;
    case 0xF5:      // PB port
      if(readyMCP2317) mcp2317.writePort(MCP_PORTB, (uint8_t) value);
    break;
    case 0xF7:      // CWR
      if(readyMCP2317) {
          if(value & 0x80) {
            if(value & 0x10) mcp2317.setPortDir(MCP_PORTA, 0xFF);
            else mcp2317.setPortDir(MCP_PORTA, 0x00);
            if(value & 0x02) mcp2317.setPortDir(MCP_PORTB, 0xFF);
            else mcp2317.setPortDir(MCP_PORTB, 0x00);
          }
      }
    break;
    default: break;
  }
};

// *************************  Arduino standard functions *********************************
void setup() {
  // Start keyboard and mouse
  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, KbdMode::GenerateVirtualKeys);
  // Initialize SPIFFS
  if(SPIFFS.begin(true)) readySPIFFS = true;
  // Initialize display (standard VGA)
  DisplayController.begin();
  DisplayController.setResolution(VGA_640x480_60Hz);

}

void loop() {
  // Application will do necessary
  app.runAsync(&DisplayController);
  vTaskDelete(NULL);
}
