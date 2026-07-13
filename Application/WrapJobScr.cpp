//******************************************************************************
//  @file WrapJobScr.cpp
//  @brief Wire Wrap guided-job screen for SmartPendant (grblHAL)
//
//  Design: all sequencing lives in the .nc job file on the CONTROLLER SD card
//  (M0 pauses + (MSG,...) prompts). This screen is a thin, operator-friendly
//  remote: pick job, press the big button, follow the prompts.
//
//  PORTING NOTE: everything that touches GrblComm goes through the
//  "GRBL API ADAPTER" block right below the includes. If a method name in
//  your checkout differs (compiler will tell you), fix it there and ONLY
//  there. Open ProgramSender.cpp / ProbeScr.cpp side by side - they use the
//  same calls with the project's real names.
//******************************************************************************

// *****************************************************************************
// ***   Includes   ************************************************************
// *****************************************************************************
#include "WrapJobScr.h"

#include <cstring>
#include <cstdio>

// *****************************************************************************
// ***   GRBL API ADAPTER   ****************************************************
// ***   Fix any name mismatches HERE ONLY.                                  ***
// *****************************************************************************

static inline GrblComm& Grbl(void) {return GrblComm::GetInstance();}

// Pendant control of the machine (MPG mode on/off).
// Alternatives seen in grblHAL pendants: GetMpgMode()/SetMpgMode(),
// IsInControl()/GainControl(), RequestModeChange().
static inline bool GrblInControl(void) {return Grbl().IsInControl();}
static inline void GrblGainControl(void) {Grbl().GainControl();}

// Simplified machine state for the wizard.
enum GrblSimpleState {GS_UNKNOWN, GS_IDLE, GS_RUN, GS_HOLD, GS_ALARM, GS_OTHER};
static GrblSimpleState GrblState(void)
{
  // Alternative enum spellings: STATE_IDLE, GRBL_STATE_IDLE, etc.
  switch(Grbl().GetState())
  {
    case GrblComm::IDLE:  return GS_IDLE;
    case GrblComm::RUN:   return GS_RUN;
    case GrblComm::JOG:   return GS_RUN;
    case GrblComm::HOLD:  return GS_HOLD;
    case GrblComm::ALARM: return GS_ALARM;
    default:              return GS_OTHER;
  }
}

// Axis positions in current work coordinates. Axis order: X=0, Y=1, Z=2, A=3.
// If your GrblComm returns positions via GetAxisPosition(axis) in units of
// 0.001 (int32), divide by 1000.0f here.
static inline float GrblPosX(void) {return Grbl().GetAxisPosition(0u);}
static inline float GrblPosA(void) {return Grbl().GetAxisPosition(3u);}

// Send a normal command line (terminator added by GrblComm if needed).
static inline void GrblSendCmd(const char* cmd)
{
  uint32_t id = 0u;
  Grbl().SendCmd(cmd, id); // alt: Grbl().SendCmd(cmd);
}

// Real-time single-byte commands (bypass the line buffer).
// alt: Grbl().CycleStart(); Grbl().Hold(); Grbl().Reset();
static inline void GrblCycleStart(void) {Grbl().SendRealTimeCmd('~');}
static inline void GrblFeedHold(void)   {Grbl().SendRealTimeCmd('!');}
static inline void GrblReset(void)      {Grbl().SendRealTimeCmd(0x18);}

// *****************************************************************************
// ***   Local helpers   *******************************************************
// *****************************************************************************

// Format a float as [-]int.frac with fixed decimals without requiring
// printf float support (newlib-nano float printf may be disabled).
static void FormatFixed(char* buf, uint32_t buf_len, float val, uint32_t decimals)
{
  int32_t mul = 1;
  for(uint32_t i = 0u; i < decimals; i++) mul *= 10;
  bool neg = (val < 0.0f);
  int32_t v = (int32_t)((neg ? -val : val) * (float)mul + 0.5f);
  if(decimals > 0u)
  {
    snprintf(buf, buf_len, "%s%ld.%0*ld", neg ? "-" : "",
             (long)(v / mul), (int)decimals, (long)(v % mul));
  }
  else
  {
    snprintf(buf, buf_len, "%s%ld", neg ? "-" : "", (long)v);
  }
}

// *****************************************************************************
// ***   GetInstance   *********************************************************
// *****************************************************************************
WrapJobScr& WrapJobScr::GetInstance()
{
  static WrapJobScr instance;
  return instance;
}

// *****************************************************************************
// ***   GrblLineSink   ********************************************************
// ***   Called from GrblComm task for EVERY received line (see the tiny    ***
// ***   GrblComm patch in INTEGRATION.md). Keep this fast: buffers only,   ***
// ***   no display calls.                                                  ***
// *****************************************************************************
void WrapJobScr::GrblLineSink(const char* line)
{
  WrapJobScr& scr = WrapJobScr::GetInstance();

  // --- Step prompts: [MSG:....] --------------------------------------------
  if(strncmp(line, "[MSG:", 5) == 0)
  {
    const char* p = line + 5;
    // Copy payload up to ']' into a temp buffer
    char tmp[2u * MSG_LINE_LEN];
    uint32_t n = 0u;
    while((*p != ']') && (*p != '\0') && (n < sizeof(tmp) - 1u)) tmp[n++] = *p++;
    tmp[n] = '\0';

    // Split into two display lines at the last space before char 38
    uint32_t split = n;
    if(n > 38u)
    {
      split = 38u;
      for(uint32_t i = 38u; i > 20u; i--)
      {
        if(tmp[i] == ' ') {split = i; break;}
      }
    }
    uint32_t l1 = (split < MSG_LINE_LEN - 1u) ? split : MSG_LINE_LEN - 1u;
    memcpy(scr.msg_line1_buf, tmp, l1);
    scr.msg_line1_buf[l1] = '\0';
    if(split < n)
    {
      const char* rest = tmp + split + ((tmp[split] == ' ') ? 1u : 0u);
      strncpy(scr.msg_line2_buf, rest, MSG_LINE_LEN - 1u);
      scr.msg_line2_buf[MSG_LINE_LEN - 1u] = '\0';
    }
    else
    {
      scr.msg_line2_buf[0] = '\0';
    }
    scr.msg_updated = true;
  }
  // --- SD file list entries: [FILE:/name.nc|SIZE:123] -----------------------
  else if(strncmp(line, "[FILE:", 6) == 0)
  {
    if(scr.file_cnt < MAX_FILES)
    {
      const char* p = line + 6;
      if(*p == '/') p++;
      char* dst = scr.file_names[scr.file_cnt];
      uint32_t n = 0u;
      while((*p != '|') && (*p != ']') && (*p != '\0') && (n < MAX_FNAME - 1u))
      {
        dst[n++] = *p++;
      }
      dst[n] = '\0';
      if(n > 0u)
      {
        scr.file_cnt++;
        if(scr.file_idx < 0) scr.file_idx = 0;
      }
    }
  }
  else
  {
    ; // all other lines: not ours
  }
}

// *****************************************************************************
// ***   Setup   ***************************************************************
// *****************************************************************************
Result WrapJobScr::Setup(int32_t y, int32_t height)
{
  scr_y = y;
  scr_h = height;
  int32_t w = display_drv.GetScreenW(); // DevCore display driver instance
                                        // (declared in DevCfg.h in stock code)

  // --- Row 1: state banner (left) -------------------------------------------
  strcpy(state_str_buf, "WIRE WRAP");
  state_str.SetParams(state_str_buf, 8, y + 4, COLOR_CYAN, String::FONT_12x16);

  // --- Row 2: job file selector < name > ------------------------------------
  btn_prev.SetParams("<", 4, y + 32, 56, 48, true);
  btn_next.SetParams(">", w - 60, y + 32, 56, 48, true);
  strcpy(file_str_buf, "no job selected");
  file_str.SetParams(file_str_buf, 70, y + 48, COLOR_WHITE, String::FONT_8x12);

  // --- Row 3: prompt box, two big lines --------------------------------------
  msg_box.SetParams(4, y + 88, w - 8, 70, COLOR_YELLOW, false);
  strcpy(msg_line1_buf, "Pick a job file, then press");
  strcpy(msg_line2_buf, "START JOB");
  msg_str1.SetParams(msg_line1_buf, 12, y + 98,  COLOR_YELLOW, String::FONT_12x16);
  msg_str2.SetParams(msg_line2_buf, 12, y + 124, COLOR_YELLOW, String::FONT_12x16);

  // --- Row 4: DRO ------------------------------------------------------------
  strcpy(dro_x_buf, "X +0.0000");
  strcpy(dro_a_buf, "A 0.0 turns");
  dro_x_str.SetParams(dro_x_buf, 8, y + 168, COLOR_GREEN, String::FONT_12x16);
  dro_a_str.SetParams(dro_a_buf, w / 2, y + 168, COLOR_GREEN, String::FONT_12x16);

  // --- Row 5: STOP (left) and big context button (right) ---------------------
  btn_stop.SetParams("STOP", 4, y + 196, 120, 60, true);
  strcpy(btn_main_buf, "START JOB");
  btn_main.SetParams(btn_main_buf, w / 2, y + 196, w / 2 - 4, 60, true);

  // Register button callbacks the same way ProbeScr does. In stock code the
  // UiButton delivers presses to the active screen's ProcessCallback() with
  // the button's address as the pointer argument.
  btn_prev.SetCallback(AppTask::GetCurrent());
  btn_next.SetCallback(AppTask::GetCurrent());
  btn_main.SetCallback(AppTask::GetCurrent());
  btn_stop.SetCallback(AppTask::GetCurrent());

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   Show   ****************************************************************
// *****************************************************************************
Result WrapJobScr::Show()
{
  // Receive every line grblHAL sends us (see GrblComm patch)
  Grbl().SetLineSink(WrapJobScr::GrblLineSink);

  // Z-order: box first, text on top, buttons on top of everything
  msg_box.Show(100u);
  state_str.Show(101u);
  file_str.Show(102u);
  msg_str1.Show(103u);
  msg_str2.Show(104u);
  dro_x_str.Show(105u);
  dro_a_str.Show(106u);
  btn_prev.Show(110u);
  btn_next.Show(111u);
  btn_main.Show(112u);
  btn_stop.Show(113u);

  // Ask the controller for its SD card file list
  if(GrblInControl())
  {
    RequestFileList();
  }

  prev_wiz_state = WIZ_ALARM; // force label refresh
  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   Hide   ****************************************************************
// *****************************************************************************
Result WrapJobScr::Hide()
{
  Grbl().SetLineSink(nullptr);

  msg_box.Hide();
  state_str.Hide();
  file_str.Hide();
  msg_str1.Hide();
  msg_str2.Hide();
  dro_x_str.Hide();
  dro_a_str.Hide();
  btn_prev.Hide();
  btn_next.Hide();
  btn_main.Hide();
  btn_stop.Hide();

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   TimerExpired   ********************************************************
// ***   Called periodically by the framework (typically every 100 ms).     ***
// *****************************************************************************
Result WrapJobScr::TimerExpired(uint32_t interval)
{
  // --- Disarm STOP confirmation after ~3 s ----------------------------------
  if(stop_armed > 0u)
  {
    static uint32_t stop_ms = 0u;
    stop_ms += interval;
    if(stop_ms > 3000u)
    {
      stop_armed = 0u;
      stop_ms = 0u;
      btn_stop.SetString("STOP"); // alt: btn_stop.SetParams(...) again
    }
  }

  // --- Wizard state machine ---------------------------------------------------
  UpdateStateMachine();

  // --- Step prompt update from RX sink ---------------------------------------
  if(msg_updated)
  {
    msg_updated = false;
    msg_str1.SetString(msg_line1_buf);
    msg_str2.SetString(msg_line2_buf);
  }

  // --- DRO --------------------------------------------------------------------
  char num[16];
  FormatFixed(num, sizeof(num), GrblPosX(), 4u);
  snprintf(dro_x_buf, sizeof(dro_x_buf), "X %s", num);
  dro_x_str.SetString(dro_x_buf);

  FormatFixed(num, sizeof(num), GrblPosA() / 360.0f, 1u);
  snprintf(dro_a_buf, sizeof(dro_a_buf), "A %s turns", num);
  dro_a_str.SetString(dro_a_buf);

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   UpdateStateMachine   **************************************************
// *****************************************************************************
void WrapJobScr::UpdateStateMachine()
{
  GrblSimpleState gs = GrblState();

  if(!GrblInControl())              wiz_state = WIZ_NO_CTRL;
  else if(gs == GS_ALARM)           wiz_state = WIZ_ALARM;
  else if(gs == GS_HOLD)            wiz_state = WIZ_PAUSED;
  else if(gs == GS_RUN)             {wiz_state = WIZ_RUNNING; job_active = true;}
  else if(gs == GS_IDLE)
  {
    if(job_active)
    {
      // Job was running and controller is idle again -> finished (M30)
      wiz_state = WIZ_DONE;
      job_active = false;
    }
    else if(wiz_state != WIZ_DONE) // stay on DONE until operator acts
    {
      wiz_state = (file_idx >= 0) ? WIZ_READY : WIZ_NO_FILE;
    }
    else
    {
      ; // remain in WIZ_DONE
    }
  }
  else
  {
    ; // GS_OTHER / GS_UNKNOWN: keep current wizard state
  }

  if(wiz_state != prev_wiz_state)
  {
    prev_wiz_state = wiz_state;
    UpdateLabels();
  }
}

// *****************************************************************************
// ***   UpdateLabels   ********************************************************
// *****************************************************************************
void WrapJobScr::UpdateLabels()
{
  const char* state_txt = "";
  const char* main_txt  = "";

  switch(wiz_state)
  {
    case WIZ_NO_CTRL:
      state_txt = "PENDANT OFF";
      main_txt  = "TAKE CTRL";
      strcpy(msg_line1_buf, "Pendant is not in control.");
      strcpy(msg_line2_buf, "Press TAKE CTRL (or MPG button).");
      msg_updated = true;
      break;
    case WIZ_NO_FILE:
      state_txt = "NO JOB FILES";
      main_txt  = "REFRESH";
      strcpy(msg_line1_buf, "No .nc files found on the");
      strcpy(msg_line2_buf, "controller SD card.");
      msg_updated = true;
      break;
    case WIZ_READY:
      state_txt = "READY";
      main_txt  = "START JOB";
      strcpy(msg_line1_buf, "Check wire spool and rod,");
      strcpy(msg_line2_buf, "then press START JOB.");
      msg_updated = true;
      break;
    case WIZ_RUNNING:
      state_txt = "WRAPPING";
      main_txt  = "HOLD";
      break;
    case WIZ_PAUSED:
      state_txt = "OPERATOR STEP";
      main_txt  = "CONTINUE";
      // msg lines already show the (MSG,...) prompt from the job file
      break;
    case WIZ_DONE:
      state_txt = "JOB DONE";
      main_txt  = "RUN AGAIN";
      strcpy(msg_line1_buf, "Remove rod and load the next");
      strcpy(msg_line2_buf, "one, then press RUN AGAIN.");
      msg_updated = true;
      break;
    case WIZ_ALARM:
      state_txt = "ALARM";
      main_txt  = "UNLOCK";
      strcpy(msg_line1_buf, "Machine alarm. Clear the fault,");
      strcpy(msg_line2_buf, "then press UNLOCK and re-zero.");
      msg_updated = true;
      break;
    default:
      break;
  }

  snprintf(state_str_buf, sizeof(state_str_buf), "%s", state_txt);
  state_str.SetString(state_str_buf);
  snprintf(btn_main_buf, sizeof(btn_main_buf), "%s", main_txt);
  btn_main.SetString(btn_main_buf); // alt: re-run btn_main.SetParams(...)
}

// *****************************************************************************
// ***   ProcessCallback   *****************************************************
// *****************************************************************************
Result WrapJobScr::ProcessCallback(const void* ptr)
{
  if(ptr == &btn_main)
  {
    HandleMainButton();
  }
  else if(ptr == &btn_stop)
  {
    if(stop_armed == 0u)
    {
      stop_armed = 1u;
      btn_stop.SetString("SURE?");
    }
    else
    {
      stop_armed = 0u;
      btn_stop.SetString("STOP");
      GrblReset();           // soft reset: kills job immediately
      job_active = false;
      wiz_state = WIZ_NO_CTRL;
      prev_wiz_state = WIZ_ALARM; // force refresh next tick
    }
  }
  else if((ptr == &btn_prev) || (ptr == &btn_next))
  {
    if((file_cnt > 0u) && (wiz_state != WIZ_RUNNING) && (wiz_state != WIZ_PAUSED))
    {
      if(ptr == &btn_next) file_idx = (file_idx + 1) % (int32_t)file_cnt;
      else file_idx = (file_idx + (int32_t)file_cnt - 1) % (int32_t)file_cnt;
      snprintf(file_str_buf, sizeof(file_str_buf), "%s", file_names[file_idx]);
      file_str.SetString(file_str_buf);
      if(wiz_state == WIZ_DONE) {wiz_state = WIZ_READY; prev_wiz_state = WIZ_ALARM;}
    }
  }
  else
  {
    ; // not our object
  }

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   HandleMainButton   ****************************************************
// *****************************************************************************
void WrapJobScr::HandleMainButton()
{
  switch(wiz_state)
  {
    case WIZ_NO_CTRL:
      GrblGainControl();
      RequestFileList();
      break;
    case WIZ_NO_FILE:
      RequestFileList();
      break;
    case WIZ_READY:
    case WIZ_DONE:
      StartSelectedJob();
      break;
    case WIZ_RUNNING:
      GrblFeedHold();
      break;
    case WIZ_PAUSED:
      GrblCycleStart();
      break;
    case WIZ_ALARM:
      GrblSendCmd("$X");
      break;
    default:
      break;
  }
}

// *****************************************************************************
// ***   RequestFileList   *****************************************************
// *****************************************************************************
void WrapJobScr::RequestFileList()
{
  file_cnt = 0u;
  file_idx = -1;
  strcpy(file_str_buf, "listing...");
  file_str.SetString(file_str_buf);
  GrblSendCmd("$F"); // grblHAL sdcard plugin: list files -> [FILE:...] lines
}

// *****************************************************************************
// ***   StartSelectedJob   ****************************************************
// *****************************************************************************
void WrapJobScr::StartSelectedJob()
{
  if((file_idx < 0) || (file_idx >= (int32_t)file_cnt)) return;

  char cmd[MAX_FNAME + 8u];
  snprintf(cmd, sizeof(cmd), "$F=/%s", file_names[file_idx]);

  strcpy(msg_line1_buf, "Starting job...");
  msg_line2_buf[0] = '\0';
  msg_updated = true;

  job_active = false;   // set true once state reports RUN
  GrblSendCmd(cmd);     // grblHAL runs the file from its own SD card
}
