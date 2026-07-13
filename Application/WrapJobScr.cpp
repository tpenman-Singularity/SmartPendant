//******************************************************************************
//  @file WrapJobScr.cpp
//  @brief Wire Wrap guided-job screen for SmartPendant (grblHAL)
//
//  Streams a .nc job file line-by-line from SD (like ProgramSender). When a
//  streamed line starts with "(MSG," the text is shown big on screen. Lines
//  with M0 pause the controller; the operator taps CONTINUE (sends Run). No
//  changes to GrblComm are required.
//******************************************************************************

// *****************************************************************************
// ***   Includes   ************************************************************
// *****************************************************************************
#include "WrapJobScr.h"
#include "Application.h"

#include "fatfs.h"
#include <cctype>
#include <cstring>
#include <cstdio>

// *****************************************************************************
// ***   Get Instance   ********************************************************
// *****************************************************************************
WrapJobScr& WrapJobScr::GetInstance()
{
  static WrapJobScr instance;
  return instance;
}

// *****************************************************************************
// ***   Private constructor: grab shared soft buttons (like ProgramSender)  **
// *****************************************************************************
WrapJobScr::WrapJobScr() : left_btn(Application::GetInstance().GetLeftButton()),
                           middle_btn(Application::GetInstance().GetMiddleButton()),
                           right_btn(Application::GetInstance().GetRightButton()) {};

// *****************************************************************************
// ***   Setup   ***************************************************************
// *****************************************************************************
Result WrapJobScr::Setup(int32_t y, int32_t height)
{
  // Fill menu_items (same as ProgramSender)
  for(uint32_t i = 0u; i < NumberOf(menu_items); i++)
  {
    menu_items[i].text = str[i];
    menu_items[i].n = sizeof(str[i]);
  }
  menu.SetCallback(AppTask::GetCurrent(), this,
                   reinterpret_cast<CallbackPtr>(ProcessMenuOkCallback),
                   reinterpret_cast<CallbackPtr>(ProcessMenuCancelCallback));
  menu.Setup(menu_items, NumberOf(menu_items), 0, y, display_drv.GetScreenW(),
             height - Font_8x12::GetInstance().GetCharH() * 2u - BORDER_W * 2);

  int32_t w = display_drv.GetScreenW();

  // Prompt frame
  msg_box.SetParams(BORDER_W, y + 8, w - BORDER_W * 2, 120, COLOR_YELLOW, false);
  // Prompt texts
  msg_title.SetParams("WIRE WRAP", BORDER_W + 8, y + 16, COLOR_CYAN,   Font_12x16::GetInstance());
  msg_line1.SetParams("Press OPEN to pick a job,",  BORDER_W + 8, y + 48, COLOR_YELLOW, Font_12x16::GetInstance());
  msg_line2.SetParams("then RUN to start.",          BORDER_W + 8, y + 74, COLOR_YELLOW, Font_12x16::GetInstance());

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   Show   ****************************************************************
// *****************************************************************************
Result WrapJobScr::Show()
{
  // Encoder handler for scrolling the file menu
  InputDrv::GetInstance().AddEncoderCallbackHandler(AppTask::GetCurrent(),
      reinterpret_cast<CallbackPtr>(ProcessEncoderCallback), this, enc_cble);

  // Prompt objects
  msg_box.Show(100);
  msg_title.Show(101);
  msg_line1.Show(101);
  msg_line2.Show(101);

  // Axis DRO: reuse Application's shared data windows (same as ProgramSender)
  for(uint32_t i = 0u; i < grbl_comm.GetLimitedNumberOfAxis(3u); i++)
  {
    DataWindow& dw_real = Application::GetInstance().GetRealDataWindow(i);
    String& dw_real_name = Application::GetInstance().GetRealDataWindowNameString(i);

    dw_real.SetParams(BORDER_W + ((display_drv.GetScreenW() - BORDER_W * 4) / 3 + BORDER_W) * i,
                      msg_box.GetEndY() + BORDER_W * 2,
                      (display_drv.GetScreenW() - BORDER_W * 4) / 3,
                      Font_8x12::GetInstance().GetCharH() * 2u, 8u,
                      grbl_comm.GetReportUnitsPrecision(i));
    dw_real.SetBorder(BORDER_W / 2, COLOR_GREY);
    dw_real.SetDataFont(Font_8x12::GetInstance());
    dw_real.SetUnits(grbl_comm.GetReportUnits(), DataWindow::RIGHT, Font_6x8::GetInstance());
    dw_real_name.SetParams(grbl_comm.GetAxisName(i), 0, 0, COLOR_WHITE, Font_6x8::GetInstance());
    dw_real_name.Move(dw_real.GetStartX() + BORDER_W, dw_real.GetStartY() + BORDER_W);
    dw_real.Show(100);
    dw_real_name.Show(100);
  }

  // Three soft buttons (same sharing scheme as ProgramSender)
  Application::GetInstance().InitSoftButtons(true);
  left_btn.SetString("Run");     left_btn.Show(102);
  middle_btn.SetString("Open");  middle_btn.Show(102);
  right_btn.SetString("Stop");   right_btn.Show(102);

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   Hide   ****************************************************************
// *****************************************************************************
Result WrapJobScr::Hide()
{
  InputDrv::GetInstance().DeleteEncoderCallbackHandler(enc_cble);

  // Close any open job file
  CloseJob();

  msg_box.Hide();
  msg_title.Hide();
  msg_line1.Hide();
  msg_line2.Hide();

  for(uint32_t i = 0u; i < GrblComm::AXIS_CNT; i++)
  {
    Application::GetInstance().GetRealDataWindow(i).Hide();
    Application::GetInstance().GetRealDataWindowNameString(i).Hide();
  }

  left_btn.Hide();
  middle_btn.Hide();
  right_btn.Hide();

  // Restore soft button sizes
  Application::GetInstance().InitSoftButtons(false);

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   ShowPrompt: update the three prompt strings   *************************
// *****************************************************************************
void WrapJobScr::ShowPrompt(const char* title, const char* l1, const char* l2)
{
  msg_title.SetString(title);
  msg_line1.SetString(l1);
  msg_line2.SetString(l2 ? l2 : "");
}

// *****************************************************************************
// ***   SetPromptFromMsgLine: parse "(MSG,....)" into title + 2 lines   *******
// *****************************************************************************
void WrapJobScr::SetPromptFromMsgLine(const char* gcode_line)
{
  // Find the text after "(MSG,"
  const char* p = strstr(gcode_line, "(MSG,");
  if(p == nullptr) return;
  p += 5; // skip "(MSG,"

  // Copy up to ')' or end into msg_buf
  uint32_t n = 0u;
  while((*p != ')') && (*p != '\0') && (*p != '\r') && (*p != '\n') && (n < MSG_BUF - 1u))
  {
    msg_buf[n++] = *p++;
  }
  msg_buf[n] = '\0';

  // Our job files format prompts as "Step X of 6 - <text>". Split at " - ".
  char title[24] = "WRAP";
  char* dash = strstr(msg_buf, " - ");
  char* body = msg_buf;
  if(dash != nullptr)
  {
    uint32_t tl = (uint32_t)(dash - msg_buf);
    if(tl > sizeof(title) - 1u) tl = sizeof(title) - 1u;
    memcpy(title, msg_buf, tl);
    title[tl] = '\0';
    body = dash + 3; // skip " - "
  }

  // Wrap body onto two lines (~28 chars per line at Font_12x16 on 480px)
  char l1[40] = {0};
  char l2[40] = {0};
  uint32_t blen = strlen(body);
  uint32_t split = blen;
  if(blen > 28u)
  {
    split = 28u;
    for(uint32_t i = 28u; i > 10u; i--) { if(body[i] == ' ') { split = i; break; } }
  }
  uint32_t c1 = (split < sizeof(l1) - 1u) ? split : sizeof(l1) - 1u;
  memcpy(l1, body, c1); l1[c1] = '\0';
  if(split < blen)
  {
    const char* rest = body + split + ((body[split] == ' ') ? 1u : 0u);
    strncpy(l2, rest, sizeof(l2) - 1u);
    l2[sizeof(l2) - 1u] = '\0';
  }

  ShowPrompt(title, l1, l2);
}

// *****************************************************************************
// ***   TimerExpired: run the streaming state machine   **********************
// *****************************************************************************
Result WrapJobScr::TimerExpired(uint32_t interval)
{
  // Button text: CONTINUE while paused at M0, else Run/Hold as usual
  if(paused_at_m0 && (grbl_comm.GetState() == GrblComm::HOLD))
  {
    left_btn.SetString("CONTINUE");
  }
  else
  {
    Application::GetInstance().UpdateLeftButtonText();
  }
  Application::GetInstance().UpdateRightButtonText();

  if(run)
  {
    // Stream while Idle/Run/Hold and in control
    if(((grbl_comm.GetState() == GrblComm::IDLE) ||
        (grbl_comm.GetState() == GrblComm::RUN)  ||
        (grbl_comm.GetState() == GrblComm::HOLD)) && grbl_comm.IsInControl())
    {
      if(finished)
      {
        if(grbl_comm.GetState() == GrblComm::IDLE)
        {
          run = false;
          ShowPrompt("JOB DONE", "Remove rod, load next,", "then RUN again.");
        }
      }
      else if(paused_at_m0)
      {
        ; // Wait: operator must tap CONTINUE (handled in ProcessCallback)
      }
      else
      {
        // Result of previous command
        GrblComm::status_t result = (id != 0u) ? grbl_comm.GetCmdResult(id) : GrblComm::Status_OK;
        if((result == GrblComm::Status_OK) || (result == GrblComm::Status_Next_Cmd_Executed))
        {
          StreamNextLine();
        }
        else if(result == GrblComm::Status_Cmd_Not_Executed_Yet)
        {
          ; // wait
        }
        else
        {
          run = false; // error -> stop
          ShowPrompt("STOPPED", "Command error.", "Check controller.");
        }
      }
    }
    else
    {
      run = false; // unexpected state -> stop
    }
  }
  else
  {
    // Not running: allow screen change, handle encoder file scroll
    Application::GetInstance().EnableScreenChange();
    if(enc_val != 0) { enc_val = 0; }
  }

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   StreamNextLine: read one line, act on it   ***************************
// *****************************************************************************
void WrapJobScr::StreamNextLine()
{
  if(!file_open) { finished = true; return; }

  char line[LINE_BUF] = {0};

  // Read next non-empty line
  for(;;)
  {
    if(f_gets(line, NumberOf(line), &job_file) == nullptr)
    {
      finished = true;   // end of file
      return;
    }
    line[NumberOf(line) - 1] = '\0';

    // Guard against over-long lines (same limit ProgramSender enforces)
    if(strlen(line) > 80u + 2u)
    {
      run = false;
      finished = true;
      ShowPrompt("STOPPED", "Line >80 chars.", "Fix the job file.");
      return;
    }

    // Trim trailing CR/LF
    uint32_t l = strlen(line);
    while(l && ((line[l-1] == '\r') || (line[l-1] == '\n'))) line[--l] = '\0';

    // Skip blank lines and pure comments starting with ';'
    if(l == 0u) continue;
    if(line[0] == ';') continue;
    break;
  }

  // If this line carries a step prompt, show it (pendant streams it, so we
  // know the step the moment we send it - no controller echo needed)
  if(strstr(line, "(MSG,") != nullptr)
  {
    SetPromptFromMsgLine(line);
  }

  // Detect an M0 program pause (word M0 not part of M0x). After sending it the
  // controller enters HOLD; we wait for the operator to tap CONTINUE.
  bool is_m0 = false;
  {
    const char* m = line;
    while((m = strchr(m, 'M')) != nullptr)
    {
      if((m[1] == '0') && !isdigit((unsigned char)m[2])) { is_m0 = true; break; }
      m++;
    }
  }

  // Send the line (append CR, like ProgramSender)
  char cmd[LINE_BUF + 2u];
  snprintf(cmd, NumberOf(cmd), "%s\r", line);
  if(grbl_comm.SendCmd(cmd, id) == Result::RESULT_OK)
  {
    if(is_m0) paused_at_m0 = true;
  }
}

// *****************************************************************************
// ***   OpenFileList: enumerate .nc/.gc files on SD (as ProgramSender does)  *
// *****************************************************************************
void WrapJobScr::OpenFileList()
{
  AppTask::GetCurrent()->StopTimer();

  CloseJob();
  BSP_SD_Init();

  FRESULT res = f_mount(&SDFatFS, (TCHAR const*)SDPath, 0);
  DIR dir;
  if(res == FR_OK) res = f_opendir(&dir, "/");

  uint32_t idx = 0u;
  if(res == FR_OK)
  {
    FILINFO fno;
    for(;;)
    {
      res = f_readdir(&dir, &fno);
      if((res != FR_OK) || (fno.fname[0] == 0)) break;

      // Match .nc* / .gc* extension (same logic as ProgramSender)
      bool add_file = false;
      uint32_t i = 0u;
      for(; i < NumberOf(fno.fname); i++) if(fno.fname[i] == '\0') break;
      for(i -= ((i >= 3u) ? 3u : 0u); i > 0; i--)
      {
        if((fno.fname[i] == '.') && (tolower(fno.fname[i+2]) == 'c'))
        {
          if((tolower(fno.fname[i+1]) == 'g') || (tolower(fno.fname[i+1]) == 'n'))
          {
            add_file = true; break;
          }
        }
      }
      if(!(fno.fattrib & AM_DIR) && add_file)
      {
        menu_items[idx].str.SetString(menu_items[idx].text, menu_items[idx].n, "%-19.19s%12lub", fno.fname, fno.fsize);
        idx++;
        if(idx == NumberOf(menu_items))
        {
          menu_items[idx - 1u].str.SetString(menu_items[idx - 1u].text, menu_items[idx - 1u].n, "-- Too many files! --");
          break;
        }
      }
    }
    f_closedir(&dir);
  }
  for(uint32_t i = idx; i < NumberOf(menu_items); i++) str[i][0] = '\0';

  AppTask::GetCurrent()->StartTimer();

  // Swap prompt view for menu
  msg_box.Hide(); msg_title.Hide(); msg_line1.Hide(); msg_line2.Hide();
  left_btn.Hide(); middle_btn.Hide(); right_btn.Hide();
  menu.SetCount(idx);
  menu.Show(100);
}

// *****************************************************************************
// ***   CloseJob   ************************************************************
// *****************************************************************************
void WrapJobScr::CloseJob()
{
  if(file_open)
  {
    f_close(&job_file);
    file_open = false;
  }
  run = false;
  finished = false;
  paused_at_m0 = false;
  id = 0u;
}

// *****************************************************************************
// ***   ProcessCallback: soft button handling   ******************************
// *****************************************************************************
Result WrapJobScr::ProcessCallback(const void* ptr)
{
  Result result = Result::RESULT_OK;

  if(ptr == &left_btn)
  {
    // CONTINUE from an M0 pause
    if(paused_at_m0 && (grbl_comm.GetState() == GrblComm::HOLD))
    {
      paused_at_m0 = false;
      grbl_comm.Run();     // ~  resume; streaming continues next tick
    }
    // Start the job (only from Idle, with a file open)
    else if(!run && file_open && grbl_comm.IsInControl() && (grbl_comm.GetState() == GrblComm::IDLE))
    {
      id = 0u;
      run = true;
      finished = false;
      paused_at_m0 = false;
      Application::GetInstance().DisableScreenChange();
    }
    else
    {
      result = Result::ERR_UNHANDLED_REQUEST; // let Application do Run/Hold
    }
  }
  else if(ptr == &right_btn)
  {
    CloseJob();
    Application::GetInstance().EnableScreenChange();
    result = Result::ERR_UNHANDLED_REQUEST; // let Application do Stop/Reset
  }
  else if((ptr == &middle_btn) && middle_btn.IsActive())
  {
    OpenFileList();
  }
  else
  {
    ; // Do nothing - MISRA rule
  }

  return result;
}

// *****************************************************************************
// ***   ProcessMenuOkCallback: a file was chosen   ***************************
// *****************************************************************************
Result WrapJobScr::ProcessMenuOkCallback(WrapJobScr* obj_ptr, void* ptr)
{
  if(obj_ptr == nullptr) return Result::ERR_NULL_PTR;
  WrapJobScr& ths = *obj_ptr;

  ths.menu.Hide();

  // Extract filename (trim trailing spaces) - same as ProgramSender
  char fn[20u] = {0};
  for(uint32_t i = 0u; i < NumberOf(fn); i++)
  {
    fn[i] = ths.menu_items[(uint32_t)ptr].text[i];
    if(fn[i] == '\0') break;
  }
  fn[NumberOf(fn) - 1] = '\0';
  for(uint32_t i = NumberOf(fn) - 1u; i > 0u; i--)
  {
    if(fn[i] <= ' ') fn[i] = '\0'; else break;
  }

  // Open the job file for line-by-line streaming
  ths.CloseJob();
  FRESULT fres = f_open(&ths.job_file, fn, FA_OPEN_EXISTING | FA_READ);
  if(fres == FR_OK)
  {
    ths.file_open = true;
    ths.ShowPrompt("READY", "Job loaded.", "Press RUN to start.");
  }
  else
  {
    ths.ShowPrompt("ERROR", "Could not open file.", "");
  }

  // Restore prompt view
  ths.msg_box.Show(100); ths.msg_title.Show(101);
  ths.msg_line1.Show(101); ths.msg_line2.Show(101);
  ths.left_btn.Show(102); ths.middle_btn.Show(102); ths.right_btn.Show(102);

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   ProcessMenuCancelCallback   ******************************************
// *****************************************************************************
Result WrapJobScr::ProcessMenuCancelCallback(WrapJobScr* obj_ptr, void* ptr)
{
  if(obj_ptr == nullptr) return Result::ERR_NULL_PTR;
  WrapJobScr& ths = *obj_ptr;

  ths.menu.Hide();
  ths.ShowPrompt("WIRE WRAP", "Open cancelled.", "Press OPEN to pick a job.");
  ths.msg_box.Show(100); ths.msg_title.Show(101);
  ths.msg_line1.Show(101); ths.msg_line2.Show(101);
  ths.left_btn.Show(102); ths.middle_btn.Show(102); ths.right_btn.Show(102);

  return Result::RESULT_OK;
}

// *****************************************************************************
// ***   ProcessEncoderCallback: scroll file menu   ***************************
// *****************************************************************************
Result WrapJobScr::ProcessEncoderCallback(WrapJobScr* obj_ptr, void* ptr)
{
  if(obj_ptr == nullptr) return Result::ERR_NULL_PTR;
  WrapJobScr& ths = *obj_ptr;
  ths.enc_val += (int32_t)ptr;
  return Result::RESULT_OK;
}
