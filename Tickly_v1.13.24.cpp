// ============================================================================
//  Tickly —— 单文件 Win32 待办工具
//
//  构建：推荐 GitHub Actions 的 MSVC 方式（windows-latest 自带的 cl.exe，
//        见 .github/workflows/build.yml）：
//    rc  /nologo /c 65001 /fo resource.res resource.rc
//    cl  /nologo /utf-8 /std:c++17 /O2 /Gy /EHsc /MT Tickly_v1.13.24.cpp \
//        resource.res /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF \
//        user32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib \
//        comdlg32.lib ole32.lib
//
//        也可用 mingw-w64（windres 必须带 --codepage=65001，否则版本信息里的
//        中文在文件属性中显示为乱码）：
//    windres --codepage=65001 resource.rc -O coff -o resource.o
//    g++ -std=c++17 -O2 -municode -mwindows -static Tickly_v1.13.24.cpp \
//        resource.o -o Tickly.exe -luser32 -lgdi32 -ladvapi32 -lshell32 \
//        -lcomctl32 -lcomdlg32
//
//        两种方式都必须让 resource.rc 参与链接：它提供「1 号图标」与
//        「DPI 感知清单 app.manifest」，缺任何一个就会出现「无图标 / 文字发虚」。
// ============================================================================
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // MSVC：不提示 localtime/_wfopen 的安全告警
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>     // GetSaveFileNameW（v1.10.0 分组导出）
#include <cstdio>        // _wfopen/fwrite
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <ctime>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000   // RegisterHotKey：按住不重复触发（Vista+）
#endif

// ---------------------------- 控件 ID --------------------------------------
static const int IDC_INPUT      = 1001;
static const int IDC_BTN_ADD    = 1002;
static const int IDC_LIST       = 1003;
// v1.13.12：底栏按钮已移除（完成靠勾选框、删除靠右键菜单）
static const int IDC_BTN_CLEAR  = 1006;   // 已并入右上角「⋯」菜单
static const int IDC_BTN_SETTINGS = 1007; // 已并入右上角「⋯」菜单
static const int IDC_BTN_MORE   = 1009;   // v1.13.11：右上角「更多」按钮

// 托盘/右键菜单命令 ID
static const int IDM_TRAY_SHOW = 3001;   // 显示主窗口
static const int IDM_TRAY_SET  = 3002;   // 设置
static const int IDM_TRAY_EXIT = 3003;   // 退出
static const int IDM_ROW_DELETE = 3004;  // 右键菜单：删除任务
static const int IDM_MULTI_DONE   = 3006; // v0.16 右键批量：完成所选
static const int IDM_MULTI_UNDONE = 3007; // v0.16 右键批量：还原所选
static const int IDM_MULTI_DELETE = 3008; // v0.16 右键批量：删除所选
static const int IDM_ROW_EDITDONE = 3005; // 右键菜单：修改完成日期（v0.15，已完成任务）
static const int IDM_APP_SETTINGS = 3011; // v1.13.11：右上角菜单 -> 设置
static const int IDM_APP_CLEAR    = 3012; // v1.13.11：右上角菜单 -> 清除已完成
static const int IDC_ROWEDIT    = 1008;  // 行内编辑框（列表子控件）
static const UINT WM_OPENDATE   = WM_APP + 2;  // 请求打开日期编辑对话框
static const UINT WM_ROWEDITCHK = WM_APP + 3;  // v0.22：编辑框首帧后复核行数
static const UINT WM_INPUTCHANGED = WM_APP + 4;  // v1.13.16：输入框内容变化的延迟处理

// 设置对话框：全局快捷键控件（v1.12.0）
static const int IDC_HK_SHOW    = 2105;  // 「显示主窗口」快捷键
static const int IDC_HK_CAPTURE = 2106;  // 「划词添加任务」快捷键
static const int IDC_REMIND_DONE = 2204; // v0.15 提醒弹窗「完成」按钮

// ---------------------------- 数据模型 -------------------------------------
struct Task {
    std::wstring text;        // 任务标题（已剥离日期时间词）
    bool         done   = false;
    long long    doneAt = 0;  // 完成时间（epoch 秒，用于分组与倒序排列）
    long long    due    = 0;  // 截止时间（epoch 秒；仅日期时为当日 00:00）
    bool         hasTime = false; // due 是否含具体时刻
    bool         notified = false; // 已弹过提醒（每个任务只提醒一次）
    bool         expanded = false; // 多行任务展开状态（仅界面状态，不持久化）
};

static std::vector<Task> g_tasks;
static std::vector<int>  g_rows;      // 显示行 -> 任务下标；负值 = 分组标题行
// v1.13.0：列表改为完全自绘子窗口（像素级平滑滚动），选中/滚动状态自管理
static int    g_selRow = -1;          // 选中的显示行（原 ListBox GetCurSel）
// v0.16 多选：g_multiSel 保存选中的任务下标（g_tasks 索引，重建行后仍有效），
// Shift+单击批量选（锚点 g_anchorRow 到单击行）、Ctrl+单击逐个切换、Ctrl+A 全选；
// 多选后右键 / 底部按钮可批量完成 / 还原 / 删除
static std::vector<int> g_multiSel;
static int g_anchorRow = -1;          // Shift 批量选锚点（最后一次普通单击的显示行）
static double g_scrollPx  = 0.0;      // 当前滚动像素（内容顶部被移出视口的高度）
static double g_scrollTgt = 0.0;      // 滚轮动画目标像素
static UINT_PTR g_scrollTimer = 0;    // 滚动动画定时器（列表窗口内 id 1）
static std::vector<int> g_rowTops;    // 每行 top y（内容坐标，size = 行数+1）
static int    g_totalH = 0;           // 内容总高（含填塞行）
static int    g_viewH  = 0;           // 列表视口高
static bool   g_sbDrag  = false;      // 正在拖动滚动条
static bool   g_sbTrack = false;      // 鼠标悬停在滚动条热区
static int    g_sbGrabOff = 0;        // 拖动抓取点相对 thumb 顶部的偏移
static bool   g_sbVis   = false;      // v0.24 thumb 是否可见（滚动/悬停唤出）
static UINT_PTR g_sbTimer = 0;        // v0.24 自动隐藏倒计时（列表窗口 id 2）
static int    g_fillRow = -1;           // 填塞行在 g_rows 中的下标（-1 = 无）
static UINT   g_fillH   = 0;            // 填塞行当前高度（像素，随窗口尺寸变化）
                                      //   -1..-5  完成分组（本周/上周/本月/上月/其他）
                                      //   -6..-10 未完成分组（v1.11.0：
                                      //           5=已逾期 6=今天 7=明天 8=更晚 9=无日期）
// v1.10.0 分组区块底部锚定：ROW_FILL 为「填塞行」，高度 = 列表剩余空间，
// 把完成分组区推到列表底部（全部折叠时贴底）；展开某分组时内容向上生长。
// -100 不会与分组标题（-1..-10）或任务下标冲突
static const int ROW_FILL = -100;
static const int ROW_DONE_SUMMARY = -11;   // v1.13.11：已完成汇总行（收纳五个子分组）
static bool g_doneCollapsed = true;        // v1.13.11：已完成区整体收纳（持久化）
// g_fillRow / g_fillH 已在滚动状态块中声明（v1.13.0）
static bool g_showWeek      = true;   // 完成分组展开状态（持久化）
static bool g_showLastWeek  = true;
static bool g_showMonth     = true;
static bool g_showLastMonth = true;
static bool g_showOther     = true;
// v0.15 未完成分组展开状态（持久化）：已逾期 / 今天 / 明天 / 更晚 / 无日期
static bool g_showOverdue   = true;
static bool g_showToday     = true;
static bool g_showTomorrow  = true;
static bool g_showIn2       = true;
static bool g_showNoDate    = true;
static int  g_grpCount[5] = {0, 0, 0, 0, 0};  // 完成分组各桶数（绘制用）
static int  g_openCount[5] = {0, 0, 0, 0, 0}; // 未完成各分组数（v1.11.0，绘制用）

// v1.13.0 字体外观设置（持久化，设置对话框可改）：
static int      g_taskFontSize = 14;                 // 任务内容字体大小（px）
static COLORREF g_taskFontCol  = RGB(29, 29, 31);    // 任务内容字体颜色
static int      g_grpFontSize  = 12;                 // 未完成分组标题字号（px）
static COLORREF g_grpFontCol   = RGB(110, 110, 115); // 未完成分组标题颜色
// v1.13.6：已完成分组（本周/上周/本月/上月/其他）单独一套字号与颜色
static int      g_doneGrpFontSize = 12;
static COLORREF g_doneGrpFontCol  = RGB(110, 110, 115);
static int      g_dtFontSize   = 12;                 // v0.26 日期时间字体大小（px）
static COLORREF g_dtFontCol    = RGB(110, 110, 115); // v0.26 日期时间字体颜色（默认与分组标题一致）

// 窗口位置记忆（tasks.txt 的 W 行）
static bool     g_wpSaved = false;
static bool     g_wpMax   = false;
static RECT     g_wpRect  {};
static UINT_PTR g_wpTimer = 0;   // 延时落盘定时器（覆盖贴靠/最大化等非拖动尺寸变化）

// 行内编辑 / 右键删除
static HWND    g_rowEdit        = nullptr;   // 行内编辑框
static int     g_rowEditRow     = -1;        // 正在编辑的显示行
static int     g_rowEditIdx     = -1;        // v0.17：正在编辑的任务下标（行高随编辑内容生长）
static int     g_rowEditLines   = 0;         // v0.17：编辑框当前内容行数（0 = 未在编辑）
static WNDPROC g_oldRowEditProc = nullptr;
static void RepositionRowEditor();          // v0.17：滚动 / 行高变化后让编辑框跟随所在行
static void KeepRowEditorInView();          // v1.13.3：编辑框被滚出视口时自动收尾
static int     g_menuRow        = -1;        // 右键菜单对应任务行
static bool g_remindOn   = true;      // 到点提醒开关（持久化）
static int  g_remindMin  = 15;        // 提前提醒分钟数（持久化）
static bool g_remindOpen = false;     // 提醒弹窗正在显示
// v0.15 提醒弹窗「完成」按钮：本次提醒的任务清单（用 due+text 定位，
// 避免弹窗期间任务增删导致下标漂移）
static std::vector<std::pair<long long, std::wstring>> g_remindItems;
static bool g_closeToTray = true;     // 点 X 后缩到托盘后台运行（设置可改，持久化）

// v1.12.0 全局快捷键（持久化）：mod = MOD_ALT/MOD_CONTROL/MOD_SHIFT 位或，
// vk = 虚拟键码；默认「显示主窗口」Alt+Q、「划词添加任务」Alt+1
static UINT g_hkShowMod = MOD_ALT;
static UINT g_hkShowVk  = 'Q';
static UINT g_hkCapMod  = MOD_ALT;
static UINT g_hkCapVk   = '1';
static bool g_hkShowOk  = false;      // 本运行期注册结果（设置对话框展示）
static bool g_hkCapOk   = false;
static HWND g_hwndToast = nullptr;    // 划词结果半透明气泡（同一时刻至多一个）
static BYTE g_toastAlpha = 242;       // 气泡当前整体透明度（淡出用）

// 任务栏托盘
static const UINT WM_TRAY = WM_APP + 1;   // 托盘图标回调消息
static UINT g_msgTaskbarCreated = 0;      // Explorer 重启后恢复图标
static bool g_nidAdded       = false;     // 托盘图标已添加
static bool g_trayTipShown   = false;     // 首次最小化气泡已显示
static bool g_trayPromoted   = false;     // 已把图标设为常驻（IsPromoted=1）
static int  g_promoteTries   = 0;         // NotifyIconSettings 子键异步建立的重试计数
static int  g_trayAddTries   = 0;         // NIM_ADD 失败重试计数
// v1.9.0 托盘心跳保活：图标可以被折叠，但绝不能消失
static int  g_aliveMiss      = 0;         // 心跳探测连续失败计数（2 次才判定丢失）
static bool g_trayAliveCheck = false;     // 提升后验证期：确认 IsPromoted 无副作用
static int  g_aliveOk        = 0;         // 验证期内连续存活轮数（满 2 轮通过）
static bool g_trayRollback   = false;     // 已回滚为系统默认（本运行期不再提升）
// v1.9.1 心跳自适应频率：验证期/异常期 5 秒快速收敛，稳定后 30 秒低频兜底
static UINT g_hbInterval     = 5000;      // 当前心跳间隔（毫秒）
static int  g_hbSteady       = 0;         // 连续正常轮数（满 6 轮降为低频）

static void UpdateButtons();  // 前置声明（早于定义使用）
static void CheckReminders(); // 前置声明（设置对话框早于定义使用）
static void OpenSettings();   // 前置声明（托盘菜单早于定义使用）
static void Layout();         // 前置声明（v1.12.0 输入框长高时重排布）
static void CenterInput();    // 前置声明（v1.12.0 输入框行数变化时重算内边距）

// ---------------------------- 全局控件 -------------------------------------
static HWND   g_hwnd      = nullptr;
static HWND   g_input     = nullptr;
static HWND   g_list      = nullptr;
static HWND   g_btnAdd    = nullptr;

static HWND   g_btnMore   = nullptr;   // v1.13.11：右上角「⋯」菜单按钮

static HFONT  g_fontTitle = nullptr;
static HFONT  g_fontSub   = nullptr;
static HFONT  g_fontGroup = nullptr;   // v1.13.0 分组标题字体（大小/颜色可设置）
static HFONT  g_fontGroupBold = nullptr; // v0.25 未完成分组标题加粗字体
static HFONT  g_fontGroupDone = nullptr; // v1.13.6 已完成分组标题字体
static HFONT  g_fontDate = nullptr;      // v0.26 日期时间字体（大小/颜色可设置）
static HFONT  g_font      = nullptr;
static HFONT  g_fontDone  = nullptr;
static HFONT  g_fontRemindT = nullptr;  // 提醒弹窗正文（大号加粗）
static HFONT  g_fontRemindB = nullptr;  // 提醒弹窗按钮（加粗）
static HFONT  g_fontToastT  = nullptr;  // v1.12.1 气泡标题（加大加粗）
static HFONT  g_fontToastB  = nullptr;  // v1.12.1 气泡正文（加大）
static HBRUSH g_brWhite   = nullptr;
static HBRUSH g_brBg      = nullptr;   // v0.27 窗口类背景刷（随主题重建）
static HPEN   g_penCheck  = nullptr;   // v1.9.1：对勾几何笔（预创建复用）

static WNDPROC g_oldEditProc = nullptr;
static bool    g_imeComposing = false;   // 输入法正在组词（期间不动格式矩形；IME 自己管理插入点）
// v1.13.18：行内编辑框（任务文本）同样需要「组词期间冻结重排」与「插入点守恒」。
// 此前这两层保护只挂在底部添加框上，于是「编辑已有多行任务、补写『后天下午』」
// 时仍会被重排打断 —— 表现为第二段上屏后插入点被甩回组合串起点，文字插到
// 『后』字前面。行内编辑框的重排路径更长（SyncRowEditorSize ->
// RebuildRowTops -> RepositionRowEditor -> MoveWindow + EM_SETRECT），
// 被清掉的风险比添加框更高。
static bool    g_rowImeComposing = false;   // 行内编辑框正在组词
// v1.13.10：主按钮悬停态（自绘按钮没有系统 hot 反馈，自己跟鼠标进出）
static bool    g_btnAddHot    = false;
static WNDPROC g_oldBtnAddProc = nullptr;
static bool    g_btnMoreHot    = false;   // v1.13.11：右上角「⋯」悬停
static WNDPROC g_oldBtnMoreProc = nullptr;

static UINT   g_dpi        = 96;
static RECT   g_listFrame  {};

static RECT   g_hintRect   {};
static std::wstring g_hintText;   // 输入框实时识别提示
static int    g_hoverRow   = -1;  // 勾选框悬停的行（-1 无）
static bool   g_hoverBox   = false; // 悬停是否落在勾选框热区
static int    g_hoverLine  = -1;  // v0.27 整行悬停的行（-1 无，与选中/勾选悬停独立）
static bool   g_trackMouse = false; // 是否已请求 WM_MOUSELEAVE 跟踪
static RECT   g_inputFrame {};    // v0.27 输入框外描边矩形（Layout 更新，父窗口绘制）

// ---------------------------- 配色 ------------------------------------------
// v0.27 主题引擎：COL_* 由编译期常量改为运行时变量，Light/Dark 两套常量
// 色板表（kPalLight/kPalDark）由 ApplyTheme() 按当前模式整表拷贝。
// Light 色板与 v0.26 逐值一致（浅色模式除 C 层四项视觉外零变化）：
// 强调色 #007AFF、深蓝 #0062CC、错误红 #FF3B30、正文 #1D1D1F、
// 次要文字 #6E6E73、分隔线 #E5E5EA、背景 #F2F2F7、选中浅灰 #EEEEEE；
// Dark 色板为 Apple 深色风（BG #1C1C1E、内容卡 #2C2C2E、强调 #0A84FF…）
static COLORREF COL_ACCENT   = RGB(0, 122, 255);    // Apple 蓝 #007AFF
static COLORREF COL_HIGHLIGHT= RGB(0, 98, 204);     // 深蓝（今天/识别提示）
static COLORREF COL_OVERDUE  = RGB(255, 59, 48);    // Apple 红 #FF3B30
static COLORREF COL_TEXT     = RGB(29, 29, 31);     // Apple label #1D1D1F
static COLORREF COL_MUTED    = RGB(110, 110, 115);  // secondary #6E6E73
static COLORREF COL_DONE     = RGB(174, 174, 178);  // 运行时派生（见 ApplyTheme）
static COLORREF COL_GRP_CNT  = RGB(174, 174, 178);  // v0.22 分组计数弱化灰（#AEAEB2）
static COLORREF COL_BORDER   = RGB(229, 229, 234);  // separator #E5E5EA
static COLORREF COL_BOX_BD   = RGB(209, 209, 214);  // #D1D1D6
static COLORREF COL_BG       = RGB(242, 242, 247);  // systemGray6 #F2F2F7
static COLORREF COL_SEL_BG   = RGB(238, 238, 238);  // v0.26 选中浅灰底 #EEEEEE
static COLORREF COL_WHITE    = RGB(255, 255, 255);  // 内容卡面 #FFFFFF（深色 #2C2C2E）
static COLORREF COL_SB       = RGB(193, 198, 206);  // v1.13.0 滚动条
static COLORREF COL_SB_HOT   = RGB(157, 163, 172);  // 滚动条悬停/拖动
static COLORREF COL_HOVER_BG = RGB(242, 242, 247);  // v0.27 整行悬停底色（随主题）

// ---------------------------- 主题引擎（v0.27 B 层） ------------------------
// 三态模式：0 = 浅色，1 = 深色，2 = 跟随系统（默认）；g_themeDark 为解析后
// 实际生效的深浅（跟随系统时读注册表 AppsUseLightTheme，0 = 系统深色）
static int  g_themeMode = 2;
static int  g_themeDark = 0;
// 用户自定义字体颜色的浅色板默认值（v1.13.0/v0.26 时代的初始值）：
static const COLORREF kDefTaskCol = RGB(29, 29, 31);
static const COLORREF kDefGrpCol  = RGB(110, 110, 115);
static const COLORREF kDefDtCol   = RGB(110, 110, 115);

struct ThemePalette {
    COLORREF accent, highlight, overdue, text, muted, grpCnt,
             border, boxBd, bg, selBg, card, sb, sbHot, hoverBg;
};
// Light：与 v0.26 各常量逐值一致
static const ThemePalette kPalLight = {
    RGB(0, 122, 255),    // accent    #007AFF
    RGB(0, 98, 204),     // highlight #0062CC
    RGB(255, 59, 48),    // overdue   #FF3B30
    RGB(29, 29, 31),     // text      #1D1D1F
    RGB(110, 110, 115),  // muted     #6E6E73
    RGB(174, 174, 178),  // grpCnt    #AEAEB2
    RGB(229, 229, 234),  // border    #E5E5EA
    RGB(209, 209, 214),  // boxBd     #D1D1D6
    RGB(242, 242, 247),  // bg        #F2F2F7
    RGB(238, 238, 238),  // selBg     #EEEEEE（v0.26 选中色）
    RGB(255, 255, 255),  // card      #FFFFFF
    RGB(193, 198, 206),  // sb        #C1C6CE
    RGB(157, 163, 172),  // sbHot     #9DA3AC
    RGB(242, 242, 247),  // hoverBg   #F2F2F7（白底行上的淡灰悬停）
};
// Dark：Apple 深色风
static const ThemePalette kPalDark = {
    RGB(10, 132, 255),   // accent    #0A84FF
    RGB(64, 156, 255),   // highlight #409CFF
    RGB(255, 69, 58),    // overdue   #FF453A
    RGB(242, 242, 247),  // text      #F2F2F7
    RGB(152, 152, 159),  // muted     #98989F
    RGB(99, 99, 102),    // grpCnt    #636366
    RGB(58, 58, 60),     // border    #3A3A3C
    RGB(72, 72, 74),     // boxBd     #48484A
    RGB(28, 28, 30),     // bg        #1C1C1E
    RGB(58, 58, 60),     // selBg     #3A3A3C
    RGB(44, 44, 46),     // card      #2C2C2E
    RGB(90, 90, 94),     // sb        #5A5A5E
    RGB(124, 124, 130),  // sbHot     #7C7C82
    RGB(51, 51, 54),     // hoverBg   #333336
};

// 线性混色：Mix(a, b, t) = a*(1-t) + b*t（四舍五入）
static COLORREF MixColor(COLORREF a, COLORREF b, double t) {
    int r = (int)(GetRValue(a) * (1.0 - t) + GetRValue(b) * t + 0.5);
    int g = (int)(GetGValue(a) * (1.0 - t) + GetGValue(b) * t + 0.5);
    int bl = (int)(GetBValue(a) * (1.0 - t) + GetBValue(b) * t + 0.5);
    return RGB(r, g, bl);
}

// 按比例加深（按钮按下态轻微加深用，pct = 加深百分比 0~100）
static COLORREF DarkenColor(COLORREF c, int pct) {
    int r = GetRValue(c) * (100 - pct) / 100;
    int g = GetGValue(c) * (100 - pct) / 100;
    int b = GetBValue(c) * (100 - pct) / 100;
    return RGB(r, g, b);
}

// 读系统「应用模式」深浅：注册表 HKCU\...\Themes\Personalize 的
// AppsUseLightTheme（DWORD，0 = 深色）；读不到按浅色
static bool SysPrefDark() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes"
                      L"\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v), t = REG_DWORD;
        LSTATUS st = RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &t,
                                      (LPBYTE)&v, &sz);
        RegCloseKey(k);
        if (st == ERROR_SUCCESS && t == REG_DWORD) return v == 0;
    }
    return false;
}

// 解析三态模式 -> 实际深浅
static void ResolveThemeMode() {
    g_themeDark = (g_themeMode == 1) ||
                  (g_themeMode == 2 && SysPrefDark());
}

// v0.27：用户自定义字体颜色的主题适配 —— 用户从未改过色（仍等于浅色板
// 默认值）时返回当前主题对应位色板值，否则保持用户颜色。
// 所有绘制与测量点必须统一走这三个辅助函数（同文字多测量点必须同源）
static COLORREF EffTaskCol() {
    return (g_taskFontCol == kDefTaskCol) ? COL_TEXT   : g_taskFontCol;
}
static COLORREF EffDoneGrpCol() {
    return (g_doneGrpFontCol == kDefGrpCol) ? COL_MUTED : g_doneGrpFontCol;
}

static COLORREF EffGrpCol() {
    return (g_grpFontCol  == kDefGrpCol ) ? COL_MUTED  : g_grpFontCol;
}
static COLORREF EffDtCol() {
    return (g_dtFontCol   == kDefDtCol  ) ? COL_MUTED  : g_dtFontCol;
}

// ---------------------------- 小工具 ----------------------------------------
static int Scale(int v) { return MulDiv(v, (int)g_dpi, 96); }

// v0.15：任务行右侧为悬浮滚动条预留的空隙 —— 滚动条 thumb 距右缘 4~11px，
// 日期 /「＋ 日期」标签整体左移让位，不再被滚动条盖住。
// 行高计算、绘制、命中测试、行内编辑几何全部使用同一函数，保证一致
static int RowGutter() { return Scale(10); }

// v1.9.1 资源优化：GDI 对象缓存。
// 界面用到的颜色总数有限，预创建后按颜色复用，不再每次绘制都反复
// 创建/销毁内核 GDI 对象。GDI 句柄为进程级资源，UI 单线程使用无并发
// 问题，进程退出自动回收。
// v0.27：缓存存储移到文件作用域 —— 主题切换时需要整体失效重建
//（两套色板 + 按钮按下态派生色的并集可能超过旧 12 槽，槽位满后新颜色
// 拿不到句柄会画不出；扩到 16 留余量）
static COLORREF s_brKey[16];  static HBRUSH s_brVal[16];  static int s_brN  = 0;
static COLORREF s_penKey[16]; static HPEN   s_penVal[16]; static int s_penN = 0;

static void InvalidateGdiCaches() {
    for (int i = 0; i < s_brN; ++i)
        if (s_brVal[i]) DeleteObject(s_brVal[i]);
    s_brN = 0;
    for (int i = 0; i < s_penN; ++i)
        if (s_penVal[i]) DeleteObject(s_penVal[i]);
    s_penN = 0;
}

static HBRUSH CachedBrush(COLORREF c) {
    for (int i = 0; i < s_brN; ++i)
        if (s_brKey[i] == c) return s_brVal[i];
    HBRUSH b = CreateSolidBrush(c);
    if (!b) return nullptr;
    if (s_brN < 16) { s_brKey[s_brN] = c; s_brVal[s_brN] = b; ++s_brN; }
    else            DeleteObject(b);          // 不会发生：颜色集合 < 16
    return b;
}

static HPEN CachedPen(COLORREF c) {
    for (int i = 0; i < s_penN; ++i)
        if (s_penKey[i] == c) return s_penVal[i];
    HPEN p = CreatePen(PS_SOLID, 1, c);
    if (!p) return nullptr;
    if (s_penN < 16) { s_penKey[s_penN] = c; s_penVal[s_penN] = p; ++s_penN; }
    else            DeleteObject(p);          // 不会发生：颜色集合 < 16
    return p;
}

static void FillSolid(HDC hdc, const RECT& rc, COLORREF c) {
    FillRect(hdc, &rc, CachedBrush(c));
}

// ---------------------------- A 层：DWM 窗口外壳（v0.27） -------------------
// Win11 圆角 + 标题栏/边框颜色随主题 + 深色标题栏。
// 动态加载 dwmapi.dll 取 DwmSetWindowAttribute：不产生导入表依赖
//（导入表保持 8 个系统 DLL 不变），加载失败（老系统）静默降级为系统样式
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static PFN_DwmSetWindowAttribute s_pfnDwmSet = nullptr;
static bool s_dwmTried = false;
// 属性号与取值手工定义（不依赖 SDK 头文件版本）：
static const DWORD kDwmwaCornerPref = 33;    // DWMWA_WINDOW_CORNER_PREFERENCE
static const DWORD kDwmwaBorderColor = 34;   // DWMWA_BORDER_COLOR
static const DWORD kDwmwaCaptionColor = 35;  // DWMWA_CAPTION_COLOR
static const DWORD kDwmwaDarkMode = 20;      // DWMWA_USE_IMMERSIVE_DARK_MODE
static const DWORD kDwmwaDarkOld  = 19;      // 老内测版本号（20 失败时回退）
static const INT    kDwmcpRound = 2;         // DWMWCP_ROUND

static void ApplyDwmTheme() {
    if (!s_dwmTried) {
        s_dwmTried = true;
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        if (m) s_pfnDwmSet = (PFN_DwmSetWindowAttribute)
                             (void*)GetProcAddress(m, "DwmSetWindowAttribute");
    }
    if (!s_pfnDwmSet || !g_hwnd) return;
    // Win11 圆角（Win10 无此属性：返回错误码，静默忽略）
    INT pref = kDwmcpRound;
    s_pfnDwmSet(g_hwnd, kDwmwaCornerPref, &pref, sizeof(pref));
    // 标题栏/边框颜色随主题（Win10 无效，静默降级为系统标题栏）
    COLORREF cap = COL_BG, border = COL_BORDER;
    s_pfnDwmSet(g_hwnd, kDwmwaCaptionColor, &cap, sizeof(cap));
    s_pfnDwmSet(g_hwnd, kDwmwaBorderColor, &border, sizeof(border));
    // 深色标题栏：主题为深色时 TRUE；先试属性 20（19041+），失败再试 19
    INT dark = g_themeDark ? 1 : 0;
    if (s_pfnDwmSet(g_hwnd, kDwmwaDarkMode, &dark, sizeof(dark)) != S_OK)
        s_pfnDwmSet(g_hwnd, kDwmwaDarkOld, &dark, sizeof(dark));
}

// v0.27 B 层：主题应用。职责清单：拷贝色板 → 派生色 → 重建全部缓存
// 画刷/画笔 → 重建窗口类背景刷 → 重设 DWM 属性 → 全窗口重绘。
// 主题切换与窗口创建（WM_CREATE）时都会调用
static void ApplyTheme() {
    ResolveThemeMode();
    const ThemePalette& p = g_themeDark ? kPalDark : kPalLight;
    COL_ACCENT    = p.accent;
    COL_HIGHLIGHT = p.highlight;
    COL_OVERDUE   = p.overdue;
    COL_TEXT      = p.text;
    COL_MUTED     = p.muted;
    COL_GRP_CNT   = p.grpCnt;
    COL_BORDER    = p.border;
    COL_BOX_BD    = p.boxBd;
    COL_BG        = p.bg;
    COL_SEL_BG    = p.selBg;
    COL_WHITE     = p.card;
    COL_SB        = p.sb;
    COL_SB_HOT    = p.sbHot;
    COL_HOVER_BG  = p.hoverBg;
    // 派生色：已完成灰 = Mix(正文, 背景, 0.68)（预混伪透明）。
    // 浅色下 ≈ #AEAEB2 与 v0.26 常量一致；深色下 ≈ #606066
    COL_DONE = MixColor(COL_TEXT, COL_BG, 0.68);
    // 重建缓存画刷/画笔与窗口类背景刷（先建新刷并接管类背景，再删旧刷，
    // 避免 DeleteObject 后类仍引用已删除句柄的窗口期）
    InvalidateGdiCaches();
    HBRUSH nb = CreateSolidBrush(COL_BG);
    if (g_hwnd && nb)
        SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)nb);
    if (g_brBg) DeleteObject(g_brBg);
    g_brBg = nb;
    // DWM 外壳属性随主题重设
    ApplyDwmTheme();
    // 全窗口重绘（主窗口 + 列表 + 输入框；行内编辑框随列表 WM_PAINT 重建）
    if (g_hwnd)  InvalidateRect(g_hwnd, nullptr, TRUE);
    if (g_list)  InvalidateRect(g_list, nullptr, FALSE);
    if (g_input) InvalidateRect(g_input, nullptr, TRUE);
}

// 勾选框热区：行左缘 ~ 40 DIP（框绘制在 left+12~left+30，热区含前后缓冲），
// 与 DrawTaskRow 的绘制保持同步
static bool PtInBoxZone(const RECT& ir, int x) {
    return x >= ir.left && x < ir.left + Scale(40);
}

static std::wstring Trim(const std::wstring& s) {
    static const wchar_t* kWs = L" \t\r\n\xA0\x3000";
    size_t b = s.find_first_not_of(kWs);
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(kWs);
    return s.substr(b, e - b + 1);
}

// v1.12.0 多行文本整理：按 \n / \r\n 拆行，逐行折叠空白、去首尾空白与空行，
// 再以 \n 连接；超长文本截断到 4000 字符。单行文本的结果与 Trim(CollapseWs())
// 完全一致（既有行为零变化）
static std::wstring CollapseWs(const std::wstring& s);   // 前置声明（定义在后）

static std::wstring NormalizeMultiline(const std::wstring& in) {
    std::wstring out;
    size_t p = 0;
    for (;;) {
        size_t e = in.find(L'\n', p);
        std::wstring line = in.substr(p,
            (e == std::wstring::npos) ? std::wstring::npos : e - p);
        line.erase(std::remove(line.begin(), line.end(), L'\r'), line.end());
        line = Trim(line);
        line = CollapseWs(line);
        if (!line.empty()) {
            if (!out.empty()) out += L'\n';
            out += line;
        }
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    if (out.size() > 4000) out.resize(4000);
    return out;
}

// v0.16：内部 \n 换行 -> EDIT 控件要求的 \r\n（打开行内编辑框载入文本用）。
// EDIT 控件只认 \r\n 为换行，纯 \n 会全部挤在一行
static std::wstring ToCrLf(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 16);
    for (wchar_t c : s) {
        if (c == L'\r') continue;      // 内部文本不应有 \r，防御性跳过
        if (c == L'\n') o += L"\r\n";
        else            o += c;
    }
    return o;
}

// tasks.txt 中任务文本的换行转义还原（仅 E\t1 标记的文件使用）：
// \\n -> 换行、\\\\ -> 反斜杠；v1.12.0 之前的历史文件不做此还原
static std::wstring UnescapeText(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[i + 1];
            if (n == L'n') { o += L'\n'; ++i; continue; }
            if (n == L'\\') { o += L'\\'; ++i; continue; }
        }
        o += s[i];
    }
    return o;
}

// 将连续空白折叠为单个空格（用于剥离日期词后的标题整理）
static std::wstring CollapseWs(const std::wstring& s) {
    std::wstring o;
    bool sp = false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xA0 || c == 0x3000) {
            sp = !o.empty();
            continue;
        }
        if (sp) { o += L' '; sp = false; }
        o += c;
    }
    return o;
}

static bool IsDigitW(wchar_t c) { return c >= L'0' && c <= L'9'; }

// 从 s[i] 起读取最多 maxDigits 位十进制数；无数字返回 -1
static int DigitAt(const std::wstring& s, size_t& i, int maxDigits) {
    int v = 0, n = 0;
    while (i < s.size() && n < maxDigits && IsDigitW(s[i])) {
        v = v * 10 + (s[i] - L'0');
        ++i; ++n;
    }
    return n ? v : -1;
}

static HFONT MakeFont(int pxHeight, bool bold = false, bool strike = false) {
    return CreateFontW(-Scale(pxHeight), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, strike ? TRUE : FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static HFONT MakeFontBold(int pxHeight) {   // 真加粗（FW_BOLD），提醒弹窗用
    return CreateFontW(-Scale(pxHeight), 0, 0, 0, FW_BOLD,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

// v1.13.24：把「尺寸随 DPI 变化」的 GDI 资源集中到一处创建/重建。
// 启动时调一次；收到 WM_DPICHANGED（拖到不同缩放比的显示器、系统缩放改了）
// 时再调一次，字体按新 DPI 重新建，文字才不会发虚。
// 注意：任务/分组/日期字体由 ApplyFontSettings() 按用户设置重建，不在这里。
static void CreateUiResources() {
    HFONT nf;
    nf = MakeFont(20, true);        // 标题
    if (g_fontTitle)   DeleteObject(g_fontTitle);   g_fontTitle   = nf;
    nf = MakeFont(12);              // 副标题
    if (g_fontSub)     DeleteObject(g_fontSub);     g_fontSub     = nf;
    nf = MakeFontBold(17);          // 提醒弹窗正文
    if (g_fontRemindT) DeleteObject(g_fontRemindT); g_fontRemindT = nf;
    nf = MakeFontBold(13);          // 提醒弹窗按钮
    if (g_fontRemindB) DeleteObject(g_fontRemindB); g_fontRemindB = nf;
    nf = MakeFontBold(15);          // v1.12.1：气泡标题（加大加粗）
    if (g_fontToastT)  DeleteObject(g_fontToastT);  g_fontToastT  = nf;
    nf = MakeFont(14);              // v1.12.1：气泡正文（加大）
    if (g_fontToastB)  DeleteObject(g_fontToastB);  g_fontToastB  = nf;
    // 对勾几何笔：宽度随 DPI（Scale(2)），颜色固定白色
    if (g_penCheck) DeleteObject(g_penCheck);
    LOGBRUSH lb { BS_SOLID, COL_WHITE, 0 };
    g_penCheck = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                              Scale(2), &lb, 0, nullptr);
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                            nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------- 时间工具 --------------------------------------
static tm LocalTmOf(long long ep) {
    time_t t = (time_t)ep;
    tm l {};
    tm* p = localtime(&t);
    if (p) l = *p;
    return l;
}

static long long MakeEpoch(int y, int mo, int d, int h, int mi) {
    tm t {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = 0;
    t.tm_isdst = -1;
    time_t r = mktime(&t);
    return (r == (time_t)-1) ? 0 : (long long)r;
}

static bool IsLeapY(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int DaysInMonth(int y, int m) {
    static const int kD[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && IsLeapY(y)) return 29;
    return (m >= 1 && m <= 12) ? kD[m - 1] : 0;
}

static long long TodayMidnight() {
    tm n = LocalTmOf((long long)time(nullptr));
    return MakeEpoch(n.tm_year + 1900, n.tm_mon + 1, n.tm_mday, 0, 0);
}

static bool IsToday(long long due) {
    if (!due) return false;
    return due >= TodayMidnight() && due < TodayMidnight() + 86400;
}

// 本周一 00:00（中国习惯：周一为一周开始）
static long long ThisWeekStart() {
    tm n = LocalTmOf((long long)time(nullptr));
    long long todayMid = MakeEpoch(n.tm_year + 1900, n.tm_mon + 1, n.tm_mday, 0, 0);
    int shift = (n.tm_wday + 6) % 7;
    return todayMid - (long long)shift * 86400;
}

// 本月 1 日 00:00
static long long ThisMonthStart() {
    tm n = LocalTmOf((long long)time(nullptr));
    return MakeEpoch(n.tm_year + 1900, n.tm_mon + 1, 1, 0, 0);
}

// 上周一 00:00
static long long LastWeekStart() {
    return ThisWeekStart() - (long long)7 * 86400;
}

// 上月 1 日 00:00
static long long LastMonthStart() {
    tm n = LocalTmOf((long long)time(nullptr));
    int y = n.tm_year + 1900, mo = n.tm_mon + 1;
    mo -= 1;
    if (mo <= 0) { mo = 12; y -= 1; }
    return MakeEpoch(y, mo, 1, 0, 0);
}

// 将 due 格式化为「今天/明天/后天/M月D日[ YYYY年]/ HH:MM」
// v0.15：逾期任务与已完成任务一律显示具体日期（M月D日[ HH:MM]，跨年含年份），
// 不再折叠成「今天 / 明天」等相对词 —— 逾期一眼看到是哪天，完成分组与
// 任务行的完成日期一致
static std::wstring FormatWhen(long long due, bool hasTime, bool done,
                               bool* overdueOut) {
    if (overdueOut) *overdueOut = false;
    if (!due) return std::wstring();
    tm d = LocalTmOf(due);
    tm n = LocalTmOf((long long)time(nullptr));
    long long dueMid = MakeEpoch(d.tm_year + 1900, d.tm_mon + 1, d.tm_mday, 0, 0);
    long long nowMid = MakeEpoch(n.tm_year + 1900, n.tm_mon + 1, n.tm_mday, 0, 0);
    long long dd = (dueMid - nowMid + 43200) / 86400;

    bool od = false;
    if (!done) {
        if (hasTime) od = (due < (long long)time(nullptr));
        else         od = (dueMid < nowMid);
    }
    if (overdueOut && !done) *overdueOut = od;

    wchar_t buf[64] = L"";
    bool specific = done || od;   // v0.15：逾期 / 已完成 -> 具体日期
    if (!specific && dd == 0)      wcscpy(buf, L"今天");
    else if (!specific && dd == 1) wcscpy(buf, L"明天");
    else if (!specific && dd == 2) wcscpy(buf, L"后天");
    else if (d.tm_year == n.tm_year)
        std::swprintf(buf, 64, L"%d月%d日", d.tm_mon + 1, d.tm_mday);
    else
        std::swprintf(buf, 64, L"%d年%d月%d日", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);

    if (hasTime) {
        wchar_t tb[16];
        std::swprintf(tb, 16, L" %02d:%02d", d.tm_hour, d.tm_min);
        wcscat(buf, tb);
    }
    return buf;
}

// 任务行右侧标签文本：已完成 = 完成日期（doneAt，与完成分组、右键
//「修改完成日期」一致，v0.15）；未完成 = 格式化截止日期时间；
// 无日期且未完成 = 「＋ 日期」占位（占位与日期标签一样可点击 -> 打开
// 日期时间设置；已完成的任务不再显示占位）
static std::wstring WhenLabelText(const Task& t) {
    if (t.done) return FormatWhen(t.doneAt, true, true, nullptr);
    if (t.due)  return FormatWhen(t.due, t.hasTime, false, nullptr);
    return std::wstring(L"\uFF0B 日期");
}

// ---------------------------- 自然语言日期时间解析 ---------------------------
struct WhenResult {
    bool        ok      = false;
    long long   due     = 0;
    bool        hasTime = false;
    bool        hasDate = false;   // v0.16：多行逐行识别时区分「仅日期/仅时间」
    std::wstring stripped;   // 剥离日期时间词后的标题
};

static const struct { const wchar_t* tok; int delta; } kDayKw[] = {
    {L"大后天", 3}, {L"后天", 2}, {L"明天", 1}, {L"明日", 1},
    {L"今天", 0}, {L"今日", 0},
};

enum TPrefix { P_NONE = 0, P_EARLY, P_MORNING, P_NOON, P_PM, P_TONIGHT, P_TOM_NIGHT };
static const struct { const wchar_t* tok; int type; } kTimePf[] = {
    {L"凌晨", P_EARLY}, {L"清晨", P_MORNING}, {L"早上", P_MORNING},
    {L"早晨", P_MORNING}, {L"上午", P_MORNING}, {L"中午", P_NOON},
    {L"午后", P_PM}, {L"下午", P_PM}, {L"傍晚", P_PM}, {L"晚上", P_PM},
    {L"夜里", P_PM}, {L"今晚", P_TONIGHT}, {L"明晚", P_TOM_NIGHT},
};

static WhenResult ParseWhen(const std::wstring& in) {
    WhenResult r;
    if (in.size() < 2) return r;
    tm now = LocalTmOf((long long)time(nullptr));
    int  nowY = now.tm_year + 1900;
    long long nowEp = (long long)time(nullptr);
    long long todayMid = TodayMidnight();

    // ---------- 1. 日期 ----------
    // v0.20：两轮扫描 —— 第一轮只找「明确日期」（具体年月日），第二轮才
    // 匹配相对词（今天/下周三等）。文本同时出现两者时，如
    // 「下周三(10月21)上午9:00开会」—— 括号里写明的具体日期才是本意
    // （口头相对词可能说错），以明确日期为准，修复 21 号被「下周三」
    // 推算结果覆盖的问题
    bool   hasDate = false;
    size_t dA = 0, dB = 0;
    int    dY = 0, dM = 0, dD = 0;
    long long dayDelta = -1;

    for (int pass = 0; pass < 2 && !hasDate; ++pass) {
      for (size_t i = 0; i < in.size(); ++i) {
        // (a) 今天/明天/后天/大后天（相对词，仅第二轮）
        if (pass == 1) {
        bool hit = false;
        for (const auto& kw : kDayKw) {
            size_t L2 = wcslen(kw.tok);
            if (in.compare(i, L2, kw.tok) == 0) {
                dayDelta = kw.delta; dA = i; dB = i + L2;
                hasDate = true; hit = true; break;
            }
        }
        if (hit) break;
        }

        // (b) (本|这|下)?(周|星期|礼拜)X / 周末（相对词，仅第二轮）
        if (pass == 1) {
        {
            size_t j = i;
            int pref = 0; // 0 无, 1 本/这, 2 下
            if (in[j] == L'本' || in[j] == L'这') { pref = 1; ++j; }
            else if (in[j] == L'下') { pref = 2; ++j; }
            int wdLen = 0;
            if (in.compare(j, 1, L"周") == 0) wdLen = 1;
            else if (j + 1 < in.size() &&
                     (in.compare(j, 2, L"星期") == 0 || in.compare(j, 2, L"礼拜") == 0))
                wdLen = 2;
            if (wdLen) {
                size_t k = j + (size_t)wdLen;
                int target = -1;
                wchar_t c = (k < in.size()) ? in[k] : 0;
                switch (c) {
                    case L'一': target = 1; break;
                    case L'二': target = 2; break;
                    case L'三': target = 3; break;
                    case L'四': target = 4; break;
                    case L'五': target = 5; break;
                    case L'六': target = 6; break;
                    case L'日': case L'天': target = 0; break;
                    case L'末': target = 6; break;   // 周末 -> 周六
                }
                if (target >= 0) {
                    ++k;
                    long long dlt = (target - now.tm_wday + 7) % 7;
                    if (pref == 2) dlt += 7;
                    dayDelta = dlt; dA = i; dB = k;
                    hasDate = true; break;
                }
            }
        }
        }

        // (b2) 「下月/下个月 + D号(D日)」（相对词，第二轮）
        //      「今年/明年 + M月D日」（明确日期，第一轮）（v1.10.1）
        {
            int mOff = 0, yrAbs = -1;
            size_t j = i;
            if (in.compare(j, 3, L"下个月") == 0)    { mOff = 1; j += 3; }
            else if (in.compare(j, 2, L"下月") == 0) { mOff = 1; j += 2; }
            else if (in.compare(j, 2, L"明年") == 0) { yrAbs = nowY + 1; j += 2; }
            else if (in.compare(j, 2, L"今年") == 0) { yrAbs = nowY; j += 2; }
            if ((mOff && pass == 1) || (yrAbs >= 0 && pass == 0)) {
                while (j < in.size() && in[j] == L' ') ++j;
                size_t k = j;
                if (mOff) {   // 下月 + 裸「D号/D日」-> 下个月该日（该月无此日则顺延）
                    int da = DigitAt(in, k, 2);
                    if (da > 0 && k < in.size() &&
                        (in[k] == L'日' || in[k] == L'号')) {
                        ++k;
                        int Y = nowY, Mo = now.tm_mon + 1 + mOff;
                        if (Mo > 12) { Mo = 1; ++Y; }
                        for (int n = 0; n < 12; ++n) {
                            if (da <= DaysInMonth(Y, Mo)) break;
                            if (++Mo > 12) { Mo = 1; ++Y; }
                        }
                        if (da <= DaysInMonth(Y, Mo)) {
                            dY = Y; dM = Mo; dD = da;
                            dA = i; dB = k; hasDate = true; break;
                        }
                    }
                } else {      // 今年/明年 + M月D日 -> 按字面年份
                    int mo = DigitAt(in, k, 2);
                    if (mo >= 1 && mo <= 12 && k < in.size() && in[k] == L'月') {
                        ++k;
                        int da = DigitAt(in, k, 2);
                        if (da > 0 && k < in.size() &&
                            (in[k] == L'日' || in[k] == L'号')) {
                            ++k;
                            if (da <= DaysInMonth(yrAbs, mo)) {
                                dY = yrAbs; dM = mo; dD = da;
                                dA = i; dB = k; hasDate = true; break;
                            }
                        }
                    }
                }
            }
        }

        // (c) 数字日期：YYYY-M-D / YYYY年M月D日（明确日期，仅第一轮）
        if (pass == 0 && IsDigitW(in[i])) {
            size_t j = i;
            int y = DigitAt(in, j, 4);
            if (y >= 1000 && j < in.size() &&
                (in[j] == L'年' || in[j] == L'-' || in[j] == L'/' || in[j] == L'.')) {
                wchar_t sep = in[j];
                size_t k = j + 1;
                int mo = DigitAt(in, k, 2);
                int da = -1;
                if (mo > 0 && k < in.size() &&
                    (sep == L'年' ? in[k] == L'月' : in[k] == sep)) {
                    ++k;
                    da = DigitAt(in, k, 2);
                    if (da > 0 && k < in.size() && (in[k] == L'日' || in[k] == L'号')) ++k;
                    if (mo >= 1 && mo <= 12 && da >= 1 && da <= DaysInMonth(y, mo) &&
                        y >= 1970 && y <= 2999) {
                        dY = y; dM = mo; dD = da;
                        dA = i; dB = k; hasDate = true; break;
                    }
                }
            }
            // M月D日
            {
                size_t j = i;
                int mo = DigitAt(in, j, 2);
                if (mo > 0 && j < in.size() && in[j] == L'月') {
                    size_t k = j + 1;
                    int da = DigitAt(in, k, 2);
                    if (da > 0 && k < in.size() && (in[k] == L'日' || in[k] == L'号')) ++k;
                    if (mo >= 1 && mo <= 12 && da >= 1 && da <= DaysInMonth(nowY, mo)) {
                        dY = 0; dM = mo; dD = da;
                        dA = i; dB = k; hasDate = true; break;
                    }
                }
            }
            // M-D / M/D（不含小数点，避免误伤 3.5 之类）
            {
                size_t j = i;
                int mo = DigitAt(in, j, 2);
                if (mo > 0 && j < in.size() && (in[j] == L'-' || in[j] == L'/')) {
                    size_t k = j + 1;
                    int da = DigitAt(in, k, 2);
                    if (mo >= 1 && mo <= 12 && da >= 1 && da <= DaysInMonth(nowY, mo)) {
                        dY = 0; dM = mo; dD = da;
                        dA = i; dB = k; hasDate = true; break;
                    }
                }
            }
            // (d) 裸「D日/D号」（明确日期，仅第一轮）（v1.10.1）：
            //     本月该日；该日已过或本月无此日 -> 逐月顺延到未来最近一次。
            //     修复「15号9点」日期段匹配不到、时间「9点」被单独识别
            //     而落到今天的问题。不匹配更长数字串的中段：「115号」
            //     不会被拆出「5号」
            if (pass == 0 && (i == 0 || !IsDigitW(in[i - 1]))) {
                size_t j = i;
                int da = DigitAt(in, j, 2);
                if (da > 0 && da <= 31 && j < in.size() &&
                    (in[j] == L'日' || in[j] == L'号')) {
                    size_t k = j + 1;
                    int Y = nowY, Mo = now.tm_mon + 1;
                    for (int n = 0; n < 12; ++n) {
                        if (da <= DaysInMonth(Y, Mo) &&
                            MakeEpoch(Y, Mo, da, 0, 0) >= todayMid) {
                            dY = Y; dM = Mo; dD = da;
                            dA = i; dB = k; hasDate = true; break;
                        }
                        if (++Mo > 12) { Mo = 1; ++Y; }
                    }
                    if (hasDate) break;
                }
            }
        }
      }
    }

    // v0.20：明确日期与相对词同现时，把紧邻的相对日期词与括号一并纳入
    // 消费区间 —— 「下周三(10月21)」日期取括号里的 10月21，剥离后标题
    // 不残留「下周三(」「）」碎片；「10月21日(下周三)」「10月21 下周三」
    // 同样处理（日期一律以明确日期为准）
    if (hasDate && dayDelta < 0) {
        auto isWkDay = [](wchar_t c) {
            switch (c) {
                case L'一': case L'二': case L'三': case L'四': case L'五':
                case L'六': case L'日': case L'天': case L'末': return true;
            }
            return false;
        };
        // 反向匹配相对日期词（词尾紧贴 end），返回词起点
        auto revRel = [&](size_t end) -> size_t {
            if (end == 0) return std::wstring::npos;
            for (const auto& kw : kDayKw) {
                size_t L2 = wcslen(kw.tok);
                if (end >= L2 && in.compare(end - L2, L2, kw.tok) == 0)
                    return end - L2;
            }
            // (上|本|这|下)?(周|星期|礼拜)X
            if (isWkDay(in[end - 1])) {
                size_t q = end - 1, ws = std::wstring::npos;
                if (q >= 1 && in[q - 1] == L'周') ws = q - 1;
                else if (q >= 2 &&
                         ((in[q - 2] == L'星' && in[q - 1] == L'期') ||
                          (in[q - 2] == L'礼' && in[q - 1] == L'拜')))
                    ws = q - 2;
                if (ws != std::wstring::npos) {
                    if (ws >= 1) {
                        wchar_t pf = in[ws - 1];
                        if (pf == L'上' || pf == L'本' ||
                            pf == L'这' || pf == L'下') --ws;
                    }
                    return ws;
                }
            }
            return std::wstring::npos;
        };
        // 正向匹配相对日期词（从 start 起），返回词尾（不含）
        auto fwdRel = [&](size_t start) -> size_t {
            if (start >= in.size()) return std::wstring::npos;
            for (const auto& kw : kDayKw) {
                size_t L2 = wcslen(kw.tok);
                if (in.compare(start, L2, kw.tok) == 0) return start + L2;
            }
            size_t j = start;
            if (j < in.size() && (in[j] == L'上' || in[j] == L'本' ||
                                  in[j] == L'这' || in[j] == L'下')) ++j;
            int wdLen = 0;
            if (j < in.size() && in[j] == L'周') wdLen = 1;
            else if (j + 1 < in.size() &&
                     (in.compare(j, 2, L"星期") == 0 ||
                      in.compare(j, 2, L"礼拜") == 0)) wdLen = 2;
            if (wdLen && j + wdLen < in.size() && isWkDay(in[j + wdLen]))
                return j + (size_t)wdLen + 1;
            return std::wstring::npos;
        };
        // 前向：日期左侧的空白/左括号后若紧贴相对词，一并消费
        //（「下周三(10月21)」「下周三 (10月21)」）
        {
            size_t p = dA;
            while (p > 0 && (in[p - 1] == L' ' || in[p - 1] == L'(' ||
                             in[p - 1] == L'（')) --p;
            if (p < dA) {
                size_t ws = revRel(p);
                if (ws != std::wstring::npos) dA = ws;
            }
        }
        // 后向：消费区间内含左括号时右括号一并消费（括号成对剥除）；
        // 否则日期右侧的空白/左括号后紧贴相对词时消费到词尾
        //（「10月21日(下周三)」「10月21 下周三」）
        {
            bool hasParen = false;
            for (size_t p2 = dA; p2 < dB && !hasParen; ++p2)
                if (in[p2] == L'(' || in[p2] == L'（') hasParen = true;
            size_t q = dB;
            if (hasParen && q < in.size() &&
                (in[q] == L')' || in[q] == L'）')) {
                ++q;
            } else {
                while (q < in.size() && (in[q] == L' ' || in[q] == L'(' ||
                                         in[q] == L'（')) ++q;
                if (q > dB) {
                    size_t e = fwdRel(q);
                    if (e != std::wstring::npos) {
                        if (e < in.size() &&
                            (in[e] == L')' || in[e] == L'）')) ++e;
                        dB = e;
                    }
                }
            }
        }
    }

    long long baseMid = 0;
    if (hasDate) {
        if (dayDelta >= 0) {
            baseMid = MakeEpoch(nowY, now.tm_mon + 1, now.tm_mday + (int)dayDelta, 0, 0);
        } else {
            int Y = dY;
            if (!Y) {
                Y = nowY;
                if (MakeEpoch(Y, dM, dD, 0, 0) < todayMid) ++Y;   // 过去日期 -> 明年
            }
            baseMid = MakeEpoch(Y, dM, dD, 0, 0);
        }
    }

    // ---------- 2. 时间 ----------
    bool   hasTime = false;
    size_t tA = 0, tB = 0;
    int    hF = 0, mF = 0, pfType = P_NONE;

    for (size_t i = 0; i < in.size();) {
        if (hasDate && i >= dA && i < dB) { i = dB; continue; }
        int  pft = P_NONE;
        size_t j = i;
        for (const auto& pf : kTimePf) {
            if (in.compare(i, 2, pf.tok) == 0) { pft = pf.type; j = i + 2; break; }
        }
        while (j < in.size() && in[j] == L' ') ++j;
        int h = DigitAt(in, j, 2);
        if (h < 0 || h > 23) { ++i; continue; }
        int mi; bool hm = false;
        if (j < in.size() && (in[j] == L':' || in[j] == L'：')) {
            ++j;
            int m = DigitAt(in, j, 2);
            if (m < 0 || m > 59) { ++i; continue; }
            mi = m; hm = true;
        } else if (j < in.size() && (in[j] == L'点' || in[j] == L'时')) {
            ++j;
            if (j < in.size() && in[j] == L'半') { mi = 30; ++j; hm = true; }
            else {
                size_t k = j;
                int m = DigitAt(in, k, 2);
                if (m >= 0 && m <= 59) {
                    j = k;
                    if (j < in.size() && in[j] == L'分') ++j;
                    mi = m; hm = true;
                } else {
                    mi = 0;
                }
            }
        } else {
            ++i; continue;
        }
        int hh = h;
        switch (pft) {
            case P_EARLY: if (hh == 12) hh = 0; break;
            case P_NOON:  if (hh <= 2) hh += 12; break;
            case P_PM: case P_TONIGHT: case P_TOM_NIGHT:
                if (hh < 12) hh += 12; break;
            default: break;
        }
        if (hh > 23) { ++i; continue; }
        // v0.16：时间范围「H:MM-H:MM / H:MM~H:MM / H:MM 至 H:MM」—— 起止
        // 一起消费，避免结束时间残留在任务标题里（如腾讯会议
        // 「会议时间：2026/10/14 14:30-15:30」此前识别后残留「-15:30」）
        {
            size_t k = j;
            while (k < in.size() && in[k] == L' ') ++k;
            static const wchar_t* kRangeSeps[] =
                { L"~", L"～", L"—", L"–", L"至", L"到", L"-" };
            size_t sepLen = 0;
            for (const wchar_t* sep : kRangeSeps) {
                size_t sl = wcslen(sep);
                if (in.compare(k, sl, sep) == 0) { sepLen = sl; break; }
            }
            if (sepLen) {
                size_t e = k + sepLen;
                while (e < in.size() && in[e] == L' ') ++e;
                int h2 = (e < in.size()) ? DigitAt(in, e, 2) : -1;
                if (h2 >= 0 && h2 <= 23 && e < in.size() &&
                    (in[e] == L':' || in[e] == L'：')) {
                    ++e;
                    int m2 = (e < in.size()) ? DigitAt(in, e, 2) : -1;
                    if (m2 >= 0 && m2 <= 59) j = e;   // 结束时间一并剥离（扩展消费区间）
                }
            }
        }
        tA = i; tB = j; hasTime = true;
        hF = hh; mF = mi; pfType = pft;
        break;
    }

    // ---------- 3. 合并 ----------
    long long due = 0;
    bool bHasTime = false;
    if (hasTime) {
        int Y, Mo, D;
        if (hasDate) {
            tm b = LocalTmOf(baseMid);
            Y = b.tm_year + 1900; Mo = b.tm_mon + 1; D = b.tm_mday;
        } else if (pfType == P_TONIGHT || pfType == P_TOM_NIGHT) {
            int add = (pfType == P_TOM_NIGHT) ? 1 : 0;
            Y = nowY; Mo = now.tm_mon + 1; D = now.tm_mday + add;
        } else {
            Y = nowY; Mo = now.tm_mon + 1; D = now.tm_mday;
        }
        due = MakeEpoch(Y, Mo, D, hF, mF);
        // 裸时间（无日期词、无上午下午修饰）若已过 -> 顺延到最近未来时刻
        if (!hasDate && pfType == P_NONE && due <= nowEp) {
            if (hF + 12 <= 23) {
                long long alt = MakeEpoch(Y, Mo, D, hF + 12, mF);
                if (alt > nowEp) due = alt;
                else due = MakeEpoch(Y, Mo, D + 1, hF, mF);
            } else {
                due = MakeEpoch(Y, Mo, D + 1, hF, mF);
            }
        }
        bHasTime = true;
    } else if (hasDate) {
        due = baseMid;
    }
    if (!due) return r;
    r.due = due;
    r.hasTime = bHasTime;
    r.hasDate = hasDate;   // v0.16
    r.ok = true;

    // ---------- 4. 剥离已识别词 ----------
    std::wstring out = in;
    auto cut = [&out](size_t a, size_t b) { out = out.substr(0, a) + out.substr(b); };
    if (hasDate && hasTime) {
        if (tA > dA) { cut(tA, tB); cut(dA, dB); }
        else         { cut(dA, dB); cut(tA, tB); }
    } else if (hasDate) {
        cut(dA, dB);
    } else if (hasTime) {
        cut(tA, tB);
    }
    // v0.20：日期/时间消费后可能留下空括号对（如「开会(10月21)」剥掉
    // 日期后剩「开会()」），清理掉保持标题整洁
    for (size_t p = out.find(L"()"); p != std::wstring::npos;
         p = out.find(L"()", p))
        out.erase(p, 2);
    for (size_t p = out.find(L"（）"); p != std::wstring::npos;
         p = out.find(L"（）", p))
        out.erase(p, 2);
    r.stripped = Trim(CollapseWs(out));
    return r;
}

// v0.16：多行文本逐行识别日期时间（划词添加用）—— 返回是否识别成功。
//  · 逐行 ParseWhen，取最优行：日期+时间同行(3) > 仅日期(2) > 仅时间(1)，
//    同优先级取最早的行。腾讯会议邀请函的
//    「会议时间：2026/10/14 14:30-15:30」即日期+时间同行命中；
//  · strippedOut：整体文本 —— 仅最优行剥离已识别词，其余行原样保留；
//    最优行剥离后只剩「会议时间：」这类标签残骸（整行以冒号结尾）时
//    整行去掉，空行不保留
static bool ParseWhenMultiline(const std::wstring& text,
                               WhenResult& out, std::wstring& strippedOut) {
    out = WhenResult();
    strippedOut.clear();
    std::vector<std::wstring> lines;
    {
        size_t p = 0;
        for (;;) {
            size_t e = text.find(L'\n', p);
            lines.push_back(text.substr(p,
                (e == std::wstring::npos) ? std::wstring::npos : e - p));
            if (e == std::wstring::npos) break;
            p = e + 1;
        }
    }
    int    bestRank = 0;
    size_t bestLine = 0;
    WhenResult best;
    for (size_t li = 0; li < lines.size(); ++li) {
        if (lines[li].empty()) continue;
        WhenResult w = ParseWhen(lines[li]);
        if (!w.ok) continue;
        int rank = (w.hasDate ? 2 : 0) + (w.hasTime ? 1 : 0);
        if (rank > bestRank) { bestRank = rank; bestLine = li; best = w; }
    }
    if (bestRank == 0) return false;
    out = best;
    for (size_t li = 0; li < lines.size(); ++li) {
        std::wstring ln = (li == bestLine) ? Trim(best.stripped) : lines[li];
        if (li == bestLine && !ln.empty()) {
            wchar_t last = ln[ln.size() - 1];
            if (last == L'：' || last == L':') continue;   // 标签残骸整行去掉
        }
        if (ln.empty()) continue;
        if (!strippedOut.empty()) strippedOut += L'\n';
        strippedOut += ln;
    }
    return true;
}

// ---------------------------- 持久化 ----------------------------------------
// 按 \t 拆分（保存时已把文本中的 \t 替换为空格，字段数稳定）
static std::vector<std::wstring> SplitAll(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t p = 0;
    for (;;) {
        size_t q = s.find(L'\t', p);
        if (q == std::wstring::npos) { out.push_back(s.substr(p)); break; }
        out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

static std::wstring GetExeDir() {
    std::wstring path(MAX_PATH * 4, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &path[0], (DWORD)path.size());
    if (n == 0 || n >= path.size()) return L".";
    path.resize(n);
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L".";
    return path.substr(0, p);
}

static std::wstring GetDataPath() {
    std::wstring portable = GetExeDir() + L"\\tasks.txt";
    HANDLE h = CreateFileW(portable.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return portable;
    }
    wchar_t appdata[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    // v1.13.7：数据目录随应用改名（TodoWin -> Tickly）；首次升级时把旧目录
    // 的数据原样复制一份过来，老用户任务不丢。旧目录保留不删，便于回退
    std::wstring dir = std::wstring(appdata) + L"\\Tickly";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\tasks.txt";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring oldPath = std::wstring(appdata) + L"\\TodoWin\\tasks.txt";
        if (GetFileAttributesW(oldPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(oldPath.c_str(), path.c_str(), FALSE);
    }
    return path;
}

static void SaveTasks() {
    std::string out = "\xEF\xBB\xBF";  // UTF-8 BOM
    // E \t 1：v1.12.0 标记行 —— 任务文本中的换行以 \\n 转义保存；
    // 旧版本读取时忽略本行（文本显示为字面 \n，不影响数据）
    out += "E\t1\r\n";
    // 分组展开状态：S \t 本周 \t 上周 \t 本月 \t 上月 \t 其他
    out += "S\t";
    out += (g_showWeek ? "1" : "0");
    out += "\t";
    out += (g_showLastWeek ? "1" : "0");
    out += "\t";
    out += (g_showMonth ? "1" : "0");
    out += "\t";
    out += (g_showLastMonth ? "1" : "0");
    out += "\t";
    out += (g_showOther ? "1" : "0");
    // v0.15：未完成分组折叠状态（旧版本读取时忽略多余字段）
    out += "\t";
    out += (g_showOverdue ? "1" : "0");
    out += "\t";
    out += (g_showToday ? "1" : "0");
    out += "\t";
    out += (g_showTomorrow ? "1" : "0");
    out += "\t";
    out += (g_showIn2 ? "1" : "0");
    out += "\t";
    out += (g_showNoDate ? "1" : "0");
    // v1.13.11：已完成区收纳状态（旧版本读取时忽略多余字段）
    out += "\t";
    out += (g_doneCollapsed ? "1" : "0");
    out += "\r\n";
    // 提醒/行为设置：C \t 提醒开关 \t 提前分钟数 \t 关闭后托盘后台运行
    out += "C\t";
    out += (g_remindOn ? "1" : "0");
    out += "\t";
    out += std::to_string(g_remindMin);
    out += "\t";
    out += (g_closeToTray ? "1" : "0");
    // v1.12.0 全局快捷键：显示主窗口 mod/vk + 划词添加 mod/vk
    out += "\t";
    out += std::to_string(g_hkShowMod);
    out += "\t";
    out += std::to_string(g_hkShowVk);
    out += "\t";
    out += std::to_string(g_hkCapMod);
    out += "\t";
    out += std::to_string(g_hkCapVk);
    out += "\r\n";
    // 窗口位置：W \t 是否最大化 \t left \t top \t right \t bottom
    if (g_hwnd) {
        WINDOWPLACEMENT wp { sizeof(wp) };
        if (GetWindowPlacement(g_hwnd, &wp)) {
            const RECT& r = wp.rcNormalPosition;
            out += "W\t";
            out += (wp.showCmd == SW_SHOWMAXIMIZED ? "1" : "0");
            out += "\t";
            out += std::to_string((long long)r.left);
            out += "\t";
            out += std::to_string((long long)r.top);
            out += "\t";
            out += std::to_string((long long)r.right);
            out += "\t";
            out += std::to_string((long long)r.bottom);
            out += "\r\n";
        }
    }
    // v1.13.0 字体外观：F \t 任务字号 \t R \t G \t B \t 分组字号 \t R \t G \t B
    //             \t 日期字号 \t R \t G \t B（v0.26 追加）
    //             \t 主题模式（v0.27 追加，旧文件缺失字段用默认值 2）
    out += "F\t";
    out += std::to_string(g_taskFontSize);
    out += "\t";
    out += std::to_string((int)GetRValue(g_taskFontCol));
    out += "\t";
    out += std::to_string((int)GetGValue(g_taskFontCol));
    out += "\t";
    out += std::to_string((int)GetBValue(g_taskFontCol));
    out += "\t";
    out += std::to_string(g_grpFontSize);
    out += "\t";
    out += std::to_string((int)GetRValue(g_grpFontCol));
    out += "\t";
    out += std::to_string((int)GetGValue(g_grpFontCol));
    out += "\t";
    out += std::to_string((int)GetBValue(g_grpFontCol));
    out += "\t";
    out += std::to_string(g_dtFontSize);
    out += "\t";
    out += std::to_string((int)GetRValue(g_dtFontCol));
    out += "\t";
    out += std::to_string((int)GetGValue(g_dtFontCol));
    out += "\t";
    out += std::to_string((int)GetBValue(g_dtFontCol));
    // v0.27：主题模式（0 = 浅色 / 1 = 深色 / 2 = 跟随系统）—— 无条件追加；
    // 旧版本读取时忽略多余字段（教训 35：追加字段写出无条件、读入有条件）
    out += "\t";
    out += std::to_string(g_themeMode);
    // v1.13.6：已完成分组字体（追加在末尾，旧版本读取时忽略多余字段）
    out += "\t";
    out += std::to_string(g_doneGrpFontSize);
    out += "\t";
    out += std::to_string((int)GetRValue(g_doneGrpFontCol));
    out += "\t";
    out += std::to_string((int)GetGValue(g_doneGrpFontCol));
    out += "\t";
    out += std::to_string((int)GetBValue(g_doneGrpFontCol));
    out += "\r\n";
    for (const Task& t : g_tasks) {
        // v1.12.0：换行以 \\n 转义保留（多行任务）；\t 仍替换为空格（字段分隔符）
        std::wstring line;
        line.reserve(t.text.size());
        for (wchar_t c : t.text) {
            if (c == L'\\')          line += L"\\\\";
            else if (c == L'\n')     line += L"\\n";
            else if (c == L'\t')     line += L' ';
            else if (c == L'\r') { } // \r 一律丢弃
            else                     line += c;
        }
        // v3: T \t done \t doneAt \t due \t hasTime \t notified \t text
        out += "T\t";
        out += (t.done ? "1" : "0");
        out += "\t";
        out += std::to_string(t.doneAt);
        out += "\t";
        out += std::to_string(t.due);
        out += "\t";
        out += (t.hasTime ? "1" : "0");
        out += "\t";
        out += (t.notified ? "1" : "0");
        out += "\t";
        out += WideToUtf8(line);
        out += "\r\n";
    }
    HANDLE h = CreateFileW(GetDataPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
}

static void LoadTasks() {
    g_tasks.clear();
    g_showWeek      = true;
    g_showLastWeek  = true;
    g_showMonth     = true;
    g_showLastMonth = true;
    g_showOther     = true;
    g_showOverdue   = true;   // v0.15 未完成分组折叠状态，默认展开
    g_showToday     = true;
    g_showTomorrow  = true;
    g_showIn2       = true;
    g_showNoDate    = true;
    g_doneCollapsed = true;   // v1.13.11：默认收纳已完成分组
    g_remindOn      = true;
    g_remindMin     = 15;
    g_closeToTray   = true;   // 旧版文件无此字段：保持默认开启
    g_hkShowMod     = MOD_ALT;   // v1.12.0 快捷键默认值
    g_hkShowVk      = 'Q';
    g_hkCapMod      = MOD_ALT;
    g_hkCapVk       = '1';
    g_themeMode     = 2;      // v0.27 主题模式默认跟随系统（文件有值则覆盖）
    g_wpSaved       = false;
    g_wpMax         = false;
    HANDLE h = CreateFileW(GetDataPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr);
    std::string data;
    if (size > 0 && size != INVALID_FILE_SIZE) {
        data.resize(size);
        DWORD got = 0;
        ReadFile(h, &data[0], size, &got, nullptr);
        data.resize(got);
    }
    CloseHandle(h);

    size_t pos = 0;
    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
    bool esc = false;   // E\t1 标记：任务文本含 \\n 转义（v1.12.0+ 写入）
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        std::string line = data.substr(pos,
            (eol == std::string::npos ? data.size() : eol) - pos);
        pos = (eol == std::string::npos) ? data.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2) continue;

        std::wstring w = Utf8ToWide(line);
        if (w.compare(0, 2, L"S\t") == 0) {
            std::vector<std::wstring> f = SplitAll(w.substr(2));
            auto on = [&](size_t i) {
                return (i < f.size() && f[i] == L"0") ? false : true;
            };
            g_showWeek      = on(0);
            g_showLastWeek  = on(1);
            g_showMonth     = on(2);
            g_showLastMonth = on(3);
            g_showOther     = on(4);
            // v0.15：未完成分组折叠状态（S 行第 6~10 字段，旧文件缺省展开）
            g_showOverdue   = on(5);
            g_showToday     = on(6);
            g_showTomorrow  = on(7);
            g_showIn2       = on(8);
            g_showNoDate    = on(9);
            // v1.13.11：已完成区收纳状态（缺失 = 收纳，即 on() 的默认 true）
            g_doneCollapsed = on(10);
            continue;
        }
        if (w.compare(0, 2, L"E\t") == 0) {
            esc = true;                 // v1.12.0：文本换行转义标记
            continue;
        }
        if (w.compare(0, 2, L"W\t") == 0) {
            // 窗口位置：是否最大化 \t left \t top \t right \t bottom
            std::vector<std::wstring> f = SplitAll(w.substr(2));
            if (f.size() >= 5) {
                RECT r;
                r.left   = (LONG)wcstol(f[1].c_str(), nullptr, 10);
                r.top    = (LONG)wcstol(f[2].c_str(), nullptr, 10);
                r.right  = (LONG)wcstol(f[3].c_str(), nullptr, 10);
                r.bottom = (LONG)wcstol(f[4].c_str(), nullptr, 10);
                if (r.right > r.left && r.bottom > r.top) {
                    g_wpRect  = r;
                    g_wpMax   = (f[0] == L"1");
                    g_wpSaved = true;
                }
            }
            continue;
        }
        if (w.compare(0, 2, L"F\t") == 0) {
            // v1.13.0：字体外观（任务字号/颜色 + 分组字号/颜色 + v0.26 日期字号/颜色），
            // 缺失字段用默认值
            std::vector<std::wstring> f = SplitAll(w.substr(2));
            auto col = [&](size_t r, size_t g, size_t b, COLORREF def) -> COLORREF {
                if (f.size() > b) {
                    int cr = (int)wcstol(f[r].c_str(), nullptr, 10);
                    int cg = (int)wcstol(f[g].c_str(), nullptr, 10);
                    int cb = (int)wcstol(f[b].c_str(), nullptr, 10);
                    if (cr < 0 || cr > 255 || cg < 0 || cg > 255 ||
                        cb < 0 || cb > 255) return def;
                    return RGB(cr, cg, cb);
                }
                return def;
            };
            if (!f.empty()) {
                int sz = (int)wcstol(f[0].c_str(), nullptr, 10);
                if (sz >= 12 && sz <= 22) g_taskFontSize = sz;
            }
            g_taskFontCol = col(1, 2, 3, g_taskFontCol);
            if (f.size() > 4) {
                int sz = (int)wcstol(f[4].c_str(), nullptr, 10);
                if (sz >= 10 && sz <= 18) g_grpFontSize = sz;
            }
            g_grpFontCol = col(5, 6, 7, g_grpFontCol);
            if (f.size() > 8) {   // v0.26：日期时间字号/颜色（旧文件缺失用默认值）
                int sz = (int)wcstol(f[8].c_str(), nullptr, 10);
                if (sz >= 10 && sz <= 18) g_dtFontSize = sz;
            }
            g_dtFontCol = col(9, 10, 11, g_dtFontCol);
            if (f.size() > 12) {   // v0.27：主题模式（F 行第 13 个字段，索引 12；
                                   // v0.26 文件只有 12 个字段，缺失用默认 2）
                int tm = (int)wcstol(f[12].c_str(), nullptr, 10);
                if (tm >= 0 && tm <= 2) g_themeMode = tm;
            }
            // v1.13.6：已完成分组字体（索引 13~16，旧文件缺失用默认值）
            if (f.size() > 13) {
                int sz = (int)wcstol(f[13].c_str(), nullptr, 10);
                if (sz >= 10 && sz <= 18) g_doneGrpFontSize = sz;
            }
            g_doneGrpFontCol = col(14, 15, 16, g_doneGrpFontCol);
            continue;
        }
        if (w.compare(0, 2, L"C\t") == 0) {
            std::vector<std::wstring> f = SplitAll(w.substr(2));
            if (f.size() >= 1) g_remindOn = (f[0] != L"0");
            if (f.size() >= 2) {
                int v = (int)wcstol(f[1].c_str(), nullptr, 10);
                if (v < 0) v = 0;
                if (v > 10080) v = 10080;
                g_remindMin = v;
            }
            if (f.size() >= 3) g_closeToTray = (f[2] != L"0");
            if (f.size() >= 7) {   // v1.12.0：两个全局快捷键 mod/vk
                // v0.15：快捷键固定 Alt 修饰（设置界面只录主键），旧文件里
                // 保存的 Ctrl/Shift 组合迁移为 Alt+主键
                UINT v1 = (UINT)wcstoul(f[4].c_str(), nullptr, 10);
                UINT v2 = (UINT)wcstoul(f[6].c_str(), nullptr, 10);
                if (v1 >= 1 && v1 <= 254) { g_hkShowMod = MOD_ALT; g_hkShowVk = v1; }
                if (v2 >= 1 && v2 <= 254) { g_hkCapMod  = MOD_ALT; g_hkCapVk  = v2; }
            }
            continue;
        }
        if (w.compare(0, 2, L"T\t") == 0) {
            std::vector<std::wstring> f = SplitAll(w.substr(2));
            Task t;
            if (f.size() >= 6) {
                // v3: done \t doneAt \t due \t hasTime \t notified \t text
                t.done     = (f[0] == L"1");
                t.doneAt   = wcstoll(f[1].c_str(), nullptr, 10);
                t.due      = wcstoll(f[2].c_str(), nullptr, 10);
                t.hasTime  = (f[3] == L"1");
                t.notified = (f[4] == L"1");
                // v1.12.0：E\t1 文件还原 \\n 转义；历史文件不做还原
                t.text     = NormalizeMultiline(esc ? UnescapeText(f[5]) : f[5]);
            } else if (f.size() == 5) {
                // v2: done \t doneAt \t due \t hasTime \t text
                t.done    = (f[0] == L"1");
                t.doneAt  = wcstoll(f[1].c_str(), nullptr, 10);
                t.due     = wcstoll(f[2].c_str(), nullptr, 10);
                t.hasTime = (f[3] == L"1");
                t.text    = NormalizeMultiline(f[4]);
            } else {
                continue;
            }
            if (!t.text.empty()) g_tasks.push_back(t);
            continue;
        }
        // v1 兼容: done \t text
        if (w[1] == L'\t' && (w[0] == L'0' || w[0] == L'1')) {
            Task t;
            t.done = (w[0] == L'1');
            t.text = NormalizeMultiline(w.substr(2));
            if (!t.text.empty()) g_tasks.push_back(t);
        }
    }
}

// ---------------------------- 行模型 ----------------------------------------
static int CountDone() {
    int n = 0;
    for (const Task& t : g_tasks) if (t.done) ++n;
    return n;
}

// 完成时间是否属于分组 grp（桶允许重叠：本周完成的任务同时计入本月完成）
// 0=本周 1=上周 2=本月（含本周） 3=上月 4=其他（不属于以上任一档）
static bool InGroupDone(long long doneAt, int grp) {
    switch (grp) {
    case 0:  return doneAt >= ThisWeekStart();
    case 1:  return doneAt >= LastWeekStart()  && doneAt < ThisWeekStart();
    case 2:  return doneAt >= ThisMonthStart();
    case 3:  return doneAt >= LastMonthStart() && doneAt < ThisMonthStart();
    default: return doneAt < LastWeekStart()   && doneAt < LastMonthStart();
    }
}

static const wchar_t* GroupName(int grp) {
    static const wchar_t* kName[5] =
        { L"本周完成", L"上周完成", L"本月完成", L"上月完成", L"其他" };
    return kName[grp];
}

// v1.11.0 未完成分组（grp 5..9）名称与计数
static const wchar_t* OpenGroupName(int grp) {
    static const wchar_t* kName[5] =
        { L"已逾期", L"今天", L"明天", L"更晚", L"无日期" };
    return kName[grp - 5];
}

// v0.15：grp 0..4 = 完成分组，5..9 = 未完成分组（已逾期/今天/明天/更晚/无日期），
// 两组都支持折叠 / 展开
static bool GroupOpen(int grp) {
    switch (grp) {
    case 0:  return g_showWeek;
    case 1:  return g_showLastWeek;
    case 2:  return g_showMonth;
    case 3:  return g_showLastMonth;
    case 4:  return g_showOther;
    case 5:  return g_showOverdue;
    case 6:  return g_showToday;
    case 7:  return g_showTomorrow;
    case 8:  return g_showIn2;
    default: return g_showNoDate;
    }
}

static void SetGroupOpen(int grp, bool v) {
    switch (grp) {
    case 0:  g_showWeek = v;      break;
    case 1:  g_showLastWeek = v;  break;
    case 2:  g_showMonth = v;     break;
    case 3:  g_showLastMonth = v; break;
    case 4:  g_showOther = v;     break;
    case 5:  g_showOverdue = v;   break;
    case 6:  g_showToday = v;     break;
    case 7:  g_showTomorrow = v;  break;
    case 8:  g_showIn2 = v;       break;
    default: g_showNoDate = v;    break;
    }
}

// ---------------------------- 多行任务的折行与行高（v1.12.0） -----------------
static const int kTaskMaxLines = 5;   // 未展开时最多显示 5 行

// 把一个词放入当前行；放不下时换行；单词本身超宽（如长 URL、无空格的
// 中文长段落）按字符硬断 —— v1.12.1 改二分查找每行最大可容纳前缀，
// 测量次数从逐字 O(N) 降到每行 O(log N)，数千字长文本行高计算不再卡顿
static void PushWrappedWord(HDC hdc, const std::wstring& word, int maxW,
                            std::wstring& cur, std::vector<std::wstring>& lines) {
    if (word.empty()) return;
    SIZE sz {};
    std::wstring cand = cur.empty() ? word : cur + L' ' + word;
    GetTextExtentPoint32W(hdc, cand.c_str(), (int)cand.size(), &sz);
    if (sz.cx <= maxW) { cur = cand; return; }
    if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
    if (word.size() == 1) { cur = word; return; }   // 单字符也超宽：只能保留
    size_t pos = 0;
    for (;;) {
        // 在 word[pos..] 上二分找最大 fit：宽度(word[pos..pos+fit)) <= maxW
        size_t lo = 1, hi = word.size() - pos, fit = 1;
        while (lo <= hi) {
            size_t mid = lo + (hi - lo) / 2;
            GetTextExtentPoint32W(hdc, word.c_str() + pos, (int)mid, &sz);
            if (sz.cx <= maxW) { fit = mid; lo = mid + 1; }
            else {
                if (mid == 0) break;
                hi = mid - 1;
            }
        }
        if (pos + fit >= word.size()) {          // 剩余部分整段放得下
            cur.assign(word, pos, std::wstring::npos);
            return;
        }
        lines.emplace_back(word, pos, fit);      // 满行断开，继续下一行
        pos += fit;
    }
}

// 任务文本按显示宽度折行：\n 为硬换行；空格为软断点（贪心装行）。
// 结果同时用于行高计算（TaskRowHeight）与绘制（DrawTaskRow），几何一致。
static std::vector<std::wstring> WrapTaskText(HDC hdc, const std::wstring& text,
                                              int maxW) {
    std::vector<std::wstring> lines;
    if (maxW < Scale(20)) maxW = Scale(20);
    size_t p = 0;
    for (;;) {
        size_t e = text.find(L'\n', p);
        std::wstring para = text.substr(p,
            (e == std::wstring::npos) ? std::wstring::npos : e - p);
        std::wstring cur;
        size_t q = 0;
        for (;;) {
            size_t r = para.find(L' ', q);
            std::wstring word = para.substr(q,
                (r == std::wstring::npos) ? std::wstring::npos : r - q);
            PushWrappedWord(hdc, word, maxW, cur, lines);
            if (r == std::wstring::npos) break;
            q = r + 1;
        }
        lines.push_back(cur);          // 段落末行（NormalizeMultiline 后不为空行）
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    return lines;
}

// 日期标签宽度（行高计算与 DrawTaskRow 用同一公式，保证几何一致）
static int WhenLabelWidth(const Task& t) {
    std::wstring when = WhenLabelText(t);
    if (when.empty()) return 0;
    HDC hdc = GetDC(g_list);
    // v0.26：与 DrawTaskRow 同源 —— 未完成任务用可设置的日期字体测量，
    // 已完成保持固定小字号（两字号不同时折行预留宽度也不同）
    HGDIOBJ ow = SelectObject(hdc, t.done ? g_fontSub : g_fontDate);
    SIZE sz {};
    GetTextExtentPoint32W(hdc, when.c_str(), (int)when.size(), &sz);
    SelectObject(hdc, ow);
    ReleaseDC(g_list, hdc);
    return sz.cx;
}

// v1.13.2：多行任务的「行高」必须与 EDIT 控件内部的行距完全一致，否则
// 显示态（自绘 DrawText 逐行排布）与编辑态（EDIT 自己排版）会按不同步长
// 累加 —— GetTextExtentPoint32 只返回 tmHeight（如 16px），而 EDIT 用的是
// tmHeight + tmExternalLeading（如 20px），10 行任务到末行就差 40px，
// 双击瞬间整段文字重新「散开」，这就是抖动的根因。
static int TaskLineHeight(HDC hdc) {
    TEXTMETRICW tm {};
    if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0)
        return (int)tm.tmHeight + (int)tm.tmExternalLeading;
    SIZE ls {};
    GetTextExtentPoint32W(hdc, L"测", 1, &ls);
    return ls.cy > 0 ? ls.cy : Scale(19);
}

// v0.23：行内编辑框的最大可视行数 —— 任务内容行数超过视口能容纳的行数时，
// 编辑框与所在行封顶为该行数（编辑框出现滚动条、行高不再随内容无限生长）；
// 内容整放得下时返回 0 = 不封顶，保持 v0.18 以来「全内容铺开」的外观。
// 容量 = (视口高 - 行上下边距 24px) / 行高，至少保 4 行（极小窗口也有可编辑
// 区）；字体按当前编辑任务的完成态选择，与 TaskRowHeight 的行高计算同源
static int RowEditMaxLines(int contentLines) {
    // v1.13.5：编辑框不再自带滚动条、也不再封顶 —— 双击即展开，编辑框高度
    // 等于展开后的整行高度，内容永远放得下；浏览长文本时滚的是整个列表
    //（与展开态同一套滚轮语义）。恒返回 0，调用点保持原样即可
    (void)contentLines;
    return 0;
}

// 任务行高度：单行任务固定 Scale(40)（与旧版一致）；
// 多行任务 = 上下内边距 + 可见行数 x 行高（未展开 > 5 行时另留「展开」按钮行）
static int TaskRowHeight(int idx) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return Scale(40);
    const Task& t = g_tasks[(size_t)idx];
    RECT lrc;
    if (!g_list || !GetClientRect(g_list, &lrc) || lrc.right <= 0) return Scale(40);
    int whenW = WhenLabelWidth(t);
    int textLeft  = Scale(12) + Scale(18) + Scale(12);
    int textRight = lrc.right - Scale(12) - RowGutter() - (whenW ? whenW + Scale(12) : 0);
    HDC hdc = GetDC(g_list);
    HGDIOBJ of = SelectObject(hdc, t.done ? g_fontDone : g_font);
    int lineH = TaskLineHeight(hdc);
    std::vector<std::wstring> lines = WrapTaskText(hdc, t.text, textRight - textLeft);
    SelectObject(hdc, of);
    ReleaseDC(g_list, hdc);
    int total = (int)lines.size();
    // v1.13.5：编辑态不再单独算高度 —— 双击即展开，编辑框只是盖在「展开态」
    // 文字上的一层，进出编辑行高逐像素相同（用户要求：编辑状态与展开选中
    // 状态高度一致，只多一个光标）。这里只保留控件实测行数优先的兜底
    if (idx == g_rowEditIdx && g_rowEdit != nullptr && g_rowEditLines > total)
        total = g_rowEditLines;   // 折行差 1 行场景（v0.20）
    // v1.12.1：行高按折行结果判定 —— 无 \n 的长文本折行后同样占多行
    if (total <= 1) return Scale(40);
    // v1.13.2：Scale(4) 是末行光标余量，留给编辑框，显示态一并计入
    int btn = (total > kTaskMaxLines) ? Scale(20) : 0;
    int vis = (t.expanded || total <= kTaskMaxLines) ? total : kTaskMaxLines;
    int h = Scale(10) + vis * lineH + Scale(10) + btn + Scale(4);
    return h > Scale(40) ? h : Scale(40);
}

// ---------------------------- 分组导出与底部锚定 -----------------------------
static const wchar_t kExportLabel[] = L"\u2193 导出";   // 分组标题右侧的导出入口

// ---------------------------- 自绘列表行几何（v1.13.0） ----------------------
// 列表已由 ListBox 替换为完全自绘子窗口：行高自己算、滚动自己控（像素级），
// 滚轮带缓动动画、滚动条为悬浮胶囊条且支持像素级拖动，双缓冲绘制无闪烁。
static int RowHeightAt(int r) {
    if (r == ROW_FILL) return (int)g_fillH;
    if (r == ROW_DONE_SUMMARY) return Scale(38);   // v1.13.11：汇总行更高一点
    if (r < 0) return Scale(30);
    return TaskRowHeight(r);
}

// 填塞行高度 = 视口高 - 其余行总高（剩余不足则为 0 -> 正常滚动）
static UINT CalcFillerHeight() {
    if (!g_list) return 0;
    RECT lrc;
    if (!GetClientRect(g_list, &lrc) || lrc.bottom <= 0) return 0;
    int others = 0;
    for (int r : g_rows) {
        if (r == ROW_FILL) continue;
        others += RowHeightAt(r);
    }
    long long fill = (long long)lrc.bottom - others;
    return fill > 0 ? (UINT)fill : 0;
}

// 重建行 top 前缀和与内容总高；填塞行高度随视口变化在这里统一处理。
// 任何行结构/行高/视口变化后都必须调用（内部处理滚动越界钳制）。
static void RebuildRowTops() {
    if (g_fillRow >= 0) g_fillH = CalcFillerHeight();
    size_t n = g_rows.size();
    g_rowTops.assign(n + 1, 0);
    int y = 0;
    for (size_t i = 0; i < n; ++i) {
        g_rowTops[i] = y;
        y += RowHeightAt(g_rows[i]);
    }
    g_rowTops[n] = y;
    g_totalH = y;
    RECT lrc;
    g_viewH = (g_list && GetClientRect(g_list, &lrc)) ? lrc.bottom : 0;
    double maxPx = (g_totalH > g_viewH) ? (double)(g_totalH - g_viewH) : 0.0;
    if (g_scrollPx > maxPx) g_scrollPx = maxPx;
    if (g_scrollTgt > maxPx) g_scrollTgt = maxPx;
}

static int RowTop(int r) {
    return (r >= 0 && r < (int)g_rowTops.size()) ? g_rowTops[r] : 0;
}

// 视口坐标 -> 显示行（含填塞行；y 超出内容底部时返回最后一行）
static int RowFromY(int viewY) {
    if (g_rowTops.empty() || g_rows.empty()) return -1;
    int y = viewY + (int)g_scrollPx;
    int n = (int)g_rows.size();
    if (y < 0) y = 0;
    if (y >= g_totalH) y = g_totalH - 1;
    int lo = 0, hi = n - 1;   // g_rowTops 单调不减，二分找最后一个 top<=y 的行
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_rowTops[mid] <= y) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// 显示行 -> 视口坐标矩形（整行，可能与视口交叠出界）
static RECT RowRect(int r) {
    RECT crc;
    if (!g_list || !GetClientRect(g_list, &crc) || crc.right <= crc.left)
        return RECT{ 0, 0, 0, 0 };
    RECT rc = { crc.left, RowTop(r) - (int)g_scrollPx,
                crc.right, RowTop(r + 1) - (int)g_scrollPx };
    return rc;
}

// 最大可滚动像素
static double MaxScrollPx() {
    return (g_totalH > g_viewH) ? (double)(g_totalH - g_viewH) : 0.0;
}

static void ListRepaint() {
    if (g_list) InvalidateRect(g_list, nullptr, FALSE);
}

// v0.24：悬浮滚动条自动隐藏 —— 任何滚动/悬停交互（ListScrollTo、热区
// 悬停、抓取拖动）都会「唤醒」thumb：立即显示并重置隐藏倒计时；约
// 0.9 秒无交互且不在拖动/悬停中时自动隐藏（Mac / Win11 悬浮滚动条习惯）
static void SbPoke() {
    if (!g_list) return;
    if (MaxScrollPx() <= 0) {            // 内容不超出视口：无可滚，保持隐藏
        if (g_sbTimer) { KillTimer(g_list, 2); g_sbTimer = 0; }
        if (g_sbVis) { g_sbVis = false; ListRepaint(); }
        return;
    }
    if (!g_sbVis) { g_sbVis = true; ListRepaint(); }
    if (g_sbTimer) KillTimer(g_list, 2);
    g_sbTimer = SetTimer(g_list, 2, 900, nullptr);
}

// 滚动到指定像素（animate=true 时启动缓动动画，false 立即到位）
static void ListScrollTo(double px, bool animate) {
    SbPoke();                            // v0.24：任何滚动都唤醒 thumb
    double maxPx = MaxScrollPx();
    if (px < 0) px = 0;
    if (px > maxPx) px = maxPx;
    g_scrollTgt = px;
    if (!animate || !g_list) {
        g_scrollPx = px;
        if (g_scrollTimer) { KillTimer(g_list, 1); g_scrollTimer = 0; }
        ListRepaint();
        RepositionRowEditor();               // v0.17：行内编辑框跟随所在行
        KeepRowEditorInView();               // v1.13.3：滚出视口则保存关闭
        return;
    }
    if (g_scrollTimer) return;          // 动画进行中：仅更新目标，帧循环收敛
    g_scrollTimer = SetTimer(g_list, 1, 15, nullptr);
}

// 让指定显示行进入视口（不可见时滚到最近边缘，带动画）
static void ListEnsureVisible(int row) {
    if (!g_list || row < 0 || row >= (int)g_rows.size()) return;
    double top = RowTop(row), bot = RowTop(row + 1);
    double px = g_scrollPx;
    if (top < px)                  px = top;
    else if (bot > px + g_viewH)   px = bot - g_viewH;
    else return;
    ListScrollTo(px, true);
}

// v1.13.1：行顶锚定 —— 双击进入编辑、展开/收起、行高变化时，让「操作的那一行」
// 在视口里的位置分毫不动。旧做法是 ListEnsureVisible（保证行底可见），多行任务
// 行高大，一进入编辑就把滚动位置往下推，整段内容向上跳、行顶甚至被推出视口，
// 表现为「双击任务文本就上移」。改为锚定行顶：内容只向下生长，滚动交给用户。
static const int kAnchorNone = -1000000;

// 任务行行顶的视口坐标（找不到返回 kAnchorNone）
static int RowAnchorY(int idx) {
    if (idx < 0) return kAnchorNone;
    for (size_t k = 0; k < g_rows.size(); ++k)
        if (g_rows[k] == idx)
            return RowTop((int)k) - (int)g_scrollPx;
    return kAnchorNone;
}

// 行结构/行高变化后把该任务行还原到 anchorY —— 通过改写滚动像素实现
static void RestoreRowAnchor(int idx, int anchorY) {
    if (idx < 0 || anchorY == kAnchorNone) return;
    for (size_t k = 0; k < g_rows.size(); ++k) {
        if (g_rows[k] != idx) continue;
        double px = (double)(RowTop((int)k) - anchorY);
        if (px < 0) px = 0;
        double maxPx = MaxScrollPx();
        if (px > maxPx) px = maxPx;
        g_scrollPx  = px;
        g_scrollTgt = px;
        if (g_scrollTimer && g_list) { KillTimer(g_list, 1); g_scrollTimer = 0; }
        ListRepaint();
        RepositionRowEditor();
        return;
    }
}

// v1.13.0：点击其他任务或空白处时自动收起已展开的任务（keepIdx 对应任务除外）
static bool CollapseExpandedExcept(int keepIdx) {
    bool changed = false;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].expanded && (int)i != keepIdx) {
            g_tasks[i].expanded = false;
            changed = true;
        }
    }
    return changed;
}

// 任务下标 -> 当前显示行（找不到返回 -1）
static int RowOfIdx(int idx) {
    for (size_t k = 0; k < g_rows.size(); ++k)
        if (g_rows[k] == idx) return (int)k;
    return -1;
}

// v1.13.0：按当前字体设置重建任务/分组字体并重算布局（启动、设置保存后调用）
static void RebuildRows();            // 前置声明（定义在后）
static void ApplyFontSettings() {
    HFONT nf;
    nf = MakeFont(g_taskFontSize);
    if (g_font) DeleteObject(g_font);
    g_font = nf;
    nf = MakeFont(g_taskFontSize, false, true);
    if (g_fontDone) DeleteObject(g_fontDone);
    g_fontDone = nf;
    nf = MakeFont(g_grpFontSize);
    if (g_fontGroup) DeleteObject(g_fontGroup);
    g_fontGroup = nf;
    nf = MakeFont(g_grpFontSize, true);  // v0.25：未完成分组标题加粗
    if (g_fontGroupBold) DeleteObject(g_fontGroupBold);
    g_fontGroupBold = nf;
    nf = MakeFont(g_doneGrpFontSize);    // v1.13.6：已完成分组独立字体
    if (g_fontGroupDone) DeleteObject(g_fontGroupDone);
    g_fontGroupDone = nf;
    nf = MakeFont(g_dtFontSize);         // v0.26：日期时间字体大小可设置
    if (g_fontDate) DeleteObject(g_fontDate);
    g_fontDate = nf;
    if (g_input) SendMessageW(g_input, WM_SETFONT, (WPARAM)g_font, TRUE);
    RebuildRows();                       // 行高/折行随字号变化，重建前缀和
    Layout();
    CenterInput();
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0],
                            n, nullptr, nullptr);
    return s;
}

// 导出分组内全部任务为 txt：每个任务一行（按完成时间倒序，与界面显示一致）
static void ExportGroup(int grp) {
    std::vector<const Task*> items;
    for (const Task& t : g_tasks)
        if (t.done && InGroupDone(t.doneAt, grp)) items.push_back(&t);
    std::stable_sort(items.begin(), items.end(),
        [](const Task* a, const Task* b) { return a->doneAt > b->doneAt; });
    if (items.empty()) return;

    std::string out = "\xEF\xBB\xBF";    // UTF-8 BOM：记事本/Excel 打开不乱码
    for (const Task* t : items) {
        out += ToUtf8(t->text);
        out += "\r\n";
    }

    wchar_t file[MAX_PATH];
    std::swprintf(file, MAX_PATH, L"%ls.txt", GroupName(grp));
    static const wchar_t kFilter[] =
        L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    OPENFILENAMEW ofn {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;               // 用户取消

    FILE* f = _wfopen(ofn.lpstrFile, L"wb");
    if (!f) {
        MessageBoxW(g_hwnd, L"文件写入失败，请检查保存位置是否可写。",
                    L"导出失败", MB_OK | MB_ICONERROR);
        return;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    wchar_t okmsg[340];
    std::swprintf(okmsg, 340, L"已导出 %d 个任务到：\n%ls",
                  (int)items.size(), ofn.lpstrFile);
    MessageBoxW(g_hwnd, okmsg, L"导出完成", MB_OK | MB_ICONINFORMATION);
}

// 单击位置是否落在分组标题右侧的「↓ 导出」上（与 DrawHeaderRow 几何一致）
static bool HeaderExportHit(int grp, const RECT& ir, int x) {
    if (grp < 0 || grp >= 5) return false;             // 仅完成分组提供导出
    if (g_grpCount[grp] <= 0) return false;            // 空分组不提供导出
    HDC hdc = GetDC(g_list);
    HGDIOBJ ow = SelectObject(hdc, g_fontGroup);   // v1.13.0：与分组标题同字体
    SIZE sz {};
    GetTextExtentPoint32W(hdc, kExportLabel, (int)wcslen(kExportLabel), &sz);
    SelectObject(hdc, ow);
    ReleaseDC(g_list, hdc);
    int right = ir.right - Scale(10);
    int left  = right - sz.cx - Scale(8);
    return x >= left && x <= right;
}

static void RebuildRows() {
    g_hoverRow = -1;   // 行结构变化，悬停状态失效
    g_hoverBox = false;
    g_rows.clear();
    g_grpCount[0] = g_grpCount[1] = g_grpCount[2] = g_grpCount[3] = g_grpCount[4] = 0;
    for (int g = 0; g < 5; ++g) g_openCount[g] = 0;
    // 未完成任务按开始日期分组（v1.11.0）：已逾期 / 今天 / 明天 / 更晚 / 无日期，
    // 空组自动隐藏。组内排序沿用 v1.9.2：有日期的按日期时间正序（同刻保持添加
    // 序），无日期的按添加时间倒序（g_tasks 数组本身即添加倒序，保持原相对顺序）
    long long todayMid = TodayMidnight();
    std::vector<int> openBuckets[5];   // 0=逾期 1=今天 2=明天 3=更晚 4=无日期
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i].done) continue;
        long long due = g_tasks[i].due;
        int g;
        if (!due)                          g = 4;
        else if (due < todayMid)           g = 0;
        else if (due < todayMid + 86400)   g = 1;
        else if (due < todayMid + 2 * 86400) g = 2;
        else                               g = 3;
        openBuckets[g].push_back((int)i);
        ++g_openCount[g];
    }
    for (int g = 0; g < 5; ++g) {
        if (g != 4)
            std::stable_sort(openBuckets[g].begin(), openBuckets[g].end(),
                [](int a, int b) {
                    return g_tasks[(size_t)a].due < g_tasks[(size_t)b].due;
                });
        if (openBuckets[g].empty()) continue;   // 分组内没有任务时自动隐藏
        g_rows.push_back(-(g + 6));             // 未完成分组标题行：-6..-10
        if (GroupOpen(g + 5))                   // v0.15：未完成分组可折叠
            for (int i : openBuckets[g]) g_rows.push_back(i);
    }
    // v1.10.0 填塞行：把分组区推到列表底部（折叠时贴底，展开向上生长）
    g_rows.push_back(ROW_FILL);
    g_fillRow = (int)g_rows.size() - 1;
    std::vector<int> buckets[5];
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (!g_tasks[i].done) continue;
        for (int g = 0; g < 5; ++g) {
            if (!InGroupDone(g_tasks[i].doneAt, g)) continue;
            buckets[g].push_back((int)i);   // 桶重叠：同一任务可进多个分组
            ++g_grpCount[g];
        }
    }
    // v1.13.11：五个已完成分组默认收成一行「已完成 N」—— 之前 5 个空分组
    // 常驻占掉列表三分之一高度，全是 0，是界面最大的视觉噪音
    g_rows.push_back(ROW_DONE_SUMMARY);        // 汇总行常驻（点击展开/收起）
    if (!g_doneCollapsed) {
        for (int g = 0; g < 5; ++g) {
            std::stable_sort(buckets[g].begin(), buckets[g].end(),
                [g](int a, int b) {
                    return g_tasks[(size_t)a].doneAt > g_tasks[(size_t)b].doneAt;
                });
            g_rows.push_back(-(g + 1));   // 分组标题行（展开后空分组也显示）
            if (GroupOpen(g))
                for (int i : buckets[g]) g_rows.push_back(i);
        }
    }
    // v1.13.0：自绘列表 —— 重建行 top 前缀和；选中行结构与 ListBox 版
    // ResetContent 一样被清空（调用方需要时自行恢复）
    g_selRow = -1;
    RebuildRowTops();
    ListRepaint();
}

// v1.12.0：重建行并尽量保留选中任务（多行展开/收起、行内编辑保存后调用）
static void RefreshListKeepSel() {
    int idx = -1;
    if (g_selRow >= 0 && g_selRow < (int)g_rows.size() && g_rows[g_selRow] >= 0)
        idx = g_rows[g_selRow];
    RebuildRows();
    if (idx >= 0) {
        int k = RowOfIdx(idx);
        if (k >= 0) g_selRow = k;
    }
    UpdateButtons();
}

// v0.15：完成分组（0..4）与未完成分组（5..9）都可折叠
static void ToggleGroup(int grp) {
    if (grp < 0 || grp >= 10) return;
    bool opening = !GroupOpen(grp);
    SetGroupOpen(grp, opening);
    SaveTasks();
    RebuildRows();
    // 完成分组区底部锚定：展开时内容向上生长出视口，此前列表停在原位，
    // 看起来像「跳回顶部」（展开的组其实在视口外）—— 现在把组标题滚到
    // 视口顶部，展开的任务直接可见；未完成分组在列表顶部、内容向下
    // 生长，无需调整滚动
    if (opening && grp < 5) {
        for (size_t r = 0; r < g_rows.size(); ++r) {
            if (g_rows[r] == -(grp + 1)) { ListScrollTo(RowTop((int)r), true); break; }
        }
    }
    UpdateButtons();
}

// ---------------------------- UI 动作 ---------------------------------------
static void RefreshList() { RebuildRows(); }

// v1.13.12：底部按钮与状态行已移除，选中态变化不再需要刷新任何外部控件
//（完成/删除改由勾选框与右键菜单承担，二者都在列表内部自绘）
static void UpdateButtons() {}

// 取消列表选中（点击列表空白处 / 分组标题 / 输入框等其他位置时）
static void DeselectList() {
    if (g_selRow == -1 && g_multiSel.empty()) return;
    g_selRow = -1;
    g_multiSel.clear();                  // v0.16：多选一并取消
    UpdateButtons();
    ListRepaint();
}

// v0.16：任务下标是否在多选集中（绘制高亮 / 右键批量判定用）
static bool MultiHas(int idx) {
    return std::find(g_multiSel.begin(), g_multiSel.end(), idx)
           != g_multiSel.end();
}

// v0.16：Shift+单击 -> [rowA, rowB] 区间内全部任务行加入多选（替换旧多选，
// 分组标题行跳过）
static void MultiSelectRange(int rowA, int rowB) {
    if (rowA > rowB) { int x = rowA; rowA = rowB; rowB = x; }
    g_multiSel.clear();
    for (int r = rowA; r <= rowB && r < (int)g_rows.size(); ++r)
        if (r >= 0 && g_rows[r] >= 0) g_multiSel.push_back(g_rows[r]);
}

// ---------------------------- 行内编辑 / 右键删除 ----------------------------
// 点击位置是否落在任务行的日期时间标签上
static bool DateLabelHit(int idx, const RECT& ir, int x) {
    const Task& t = g_tasks[(size_t)idx];
    std::wstring when = WhenLabelText(t);   // 无日期任务命中「＋ 日期」占位
    if (when.empty()) return false;
    HDC hdc = GetDC(g_list);
    HGDIOBJ ow = SelectObject(hdc, t.done ? g_fontSub : g_fontDate);   // v0.26 与绘制同源
    SIZE sz {};
    GetTextExtentPoint32W(hdc, when.c_str(), (int)when.size(), &sz);
    SelectObject(hdc, ow);
    ReleaseDC(g_list, hdc);
    int right = ir.right - Scale(6) - RowGutter();
    int left  = right - sz.cx - Scale(6);
    return x >= left && x <= right;
}

// v1.12.0：单击是否落在多行任务的「展开 / 收起」按钮上（与 DrawTaskRow 几何一致）
static bool ExpandBtnHit(int idx, const RECT& ir, int x, int y) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return false;
    if (idx == g_rowEditIdx && g_rowEdit) return false;   // v0.17：编辑中按钮被编辑框覆盖
    const Task& t = g_tasks[(size_t)idx];
    int whenW = WhenLabelWidth(t);
    int textRight = ir.right - Scale(12) - RowGutter() - (whenW ? whenW + Scale(12) : 0);
    HDC hdc = GetDC(g_list);
    HGDIOBJ of = SelectObject(hdc, t.done ? g_fontDone : g_font);
    // v1.12.1：按钮存在性按折行结果判定（无 \n 的长文本折行超 5 行同样展开/收起）
    std::vector<std::wstring> lines = WrapTaskText(hdc, t.text,
                                                   textRight - (ir.left + Scale(12) + Scale(18) + Scale(12)));
    if ((int)lines.size() <= kTaskMaxLines) {
        SelectObject(hdc, of);
        ReleaseDC(g_list, hdc);
        return false;
    }
    const wchar_t* label = t.expanded ? L"\u25B4 收起" : L"\u25BE 展开";
    SIZE sz {};
    GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &sz);
    SelectObject(hdc, of);
    ReleaseDC(g_list, hdc);
    int left = textRight - sz.cx;
    // v1.13.5：与绘制同源 —— 行底右下角，行底出视口时吸附到视口底部
    int top = ir.bottom - Scale(6) - Scale(17);
    bool floating = false;
    if (g_viewH > Scale(40) && top + Scale(17) > g_viewH - Scale(4)) {
        top = g_viewH - Scale(4) - Scale(17);
        floating = true;
    }
    if (top < ir.top + Scale(4)) top = ir.top + Scale(4);
    int bot = top + Scale(17);
    if (floating) {          // v1.13.13：胶囊底比文字大一圈，命中区同步扩大
        left -= Scale(8);
        if (left < ir.left) left = ir.left;
        top  -= Scale(4);
        bot  += Scale(4);
    }
    return x >= left && x <= textRight && y >= top && y <= bot;
}

// 关闭行内编辑框（save=true 保存修改；文本不变则跳过写盘）
static void CloseRowEditor(bool save) {
    if (!g_rowEdit) return;
    HWND ed = g_rowEdit;
    int  row = g_rowEditRow;
    g_rowEdit      = nullptr;
    g_rowEditRow   = -1;
    g_rowEditIdx   = -1;   // v0.17：编辑态行高覆盖一并解除
    g_rowEditLines = 0;
    if (save && row >= 0 && row < (int)g_rows.size() && g_rows[row] >= 0) {
        int len = GetWindowTextLengthW(ed);
        std::wstring buf;
        buf.resize((size_t)len + 1);
        GetWindowTextW(ed, &buf[0], len + 1);
        buf.resize(wcslen(buf.c_str()));
        std::wstring text = NormalizeMultiline(buf);
        Task& t = g_tasks[(size_t)g_rows[row]];
        if (!text.empty() && text != t.text) {
            // 编辑文本中若写了日期时间词（如「明天 15:00」）则一并识别为任务属性
            //（仅单行文本参与识别；多行文本整段保留，不做日期解析）
            if (text.find(L'\n') == std::wstring::npos) {
                WhenResult w = ParseWhen(text);
                if (w.ok && !w.stripped.empty()) {
                    t.text     = w.stripped;
                    t.due      = w.due;
                    t.hasTime  = w.hasTime;
                    t.notified = false;      // 时间变更后允许再次提醒
                } else {
                    t.text = text;
                }
            } else {
                t.text = text;
            }
            SaveTasks();
            RefreshListKeepSel();   // v1.12.0：文本变了行高可能变，重建量高
        }
    }
    SetWindowLongPtrW(ed, GWLP_WNDPROC, (LONG_PTR)g_oldRowEditProc);
    DestroyWindow(ed);
    UpdateButtons();
    // v1.13.4：无条件重建行 —— 编辑态行高是「视口满高」，Esc/未改动关闭时
    // 若不重建，g_rowTops 会一直停在编辑态高度，整行留出一大块空白
    //（此前只有「文本变了」的分支才重建）
    RefreshListKeepSel();
    InvalidateRect(g_list, nullptr, TRUE);
    if (g_input) SetFocus(g_input);
}

// ---------------------------- 蓝色光标（自绘 caret，v1.13.1） -----------------
// Win32 的 EDIT 光标颜色取自系统设置（默认黑），没有可改色的公开 API：
// CreateCaret 的位图是单色掩码（只能「反色 / 屏幕色」），SPI_SETCARETWIDTH 只
// 管宽度。这里隐藏系统光标，在插入点位置自绘一条强调蓝竖线，定时器维持
// 530ms 的闪烁节奏（与系统光标速率一致）。竖线随控件重绘重新落位，因此
// 每次相位切换都整框重绘一次 —— 编辑框只有几行文字，代价可忽略。
static const COLORREF kCaretCol = RGB(0, 122, 255);   // 与 COL_ACCENT 同源
static HWND     g_caretHwnd  = nullptr;   // 当前接管光标的编辑框
static UINT_PTR g_caretTimer = 0;         // 闪烁定时器（编辑框内 id 3）
static bool     g_caretOn    = true;      // 闪烁相位

static void DetachBlueCaret();

// 插入点竖线矩形（客户区坐标）；无焦点 / 光标滚出可视区时返回空矩形
static RECT BlueCaretRect(HWND h) {
    RECT none { 0, 0, 0, 0 };
    if (!h || !IsWindow(h) || GetFocus() != h) return none;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    // v1.13.15：取选区终点 —— 普通编辑时 s0==s1 无差别；调用中文输入法组词
    // 期间 s0 落在「组合串开头」，用它定位会把蓝线画到刚敲的第一个字前面，
    // 用户看到的正是「光标跑到『后』字前面」。s1 是组合串末尾，与系统插入点
    // 的实际视觉位置一致
    s0 = s1;
    int x = 0, y = 0;
    bool located = false;
    LRESULT p = SendMessageW(h, EM_POSFROMCHAR, (WPARAM)(int)s0, 0);
    if (p != -1) {
        x = (int)(short)LOWORD(p);
        y = (int)(short)HIWORD(p);
        located = true;
    } else if (s0 > 0) {
        // v1.13.15：插入点在文本末尾时 EM_POSFROMCHAR 常返回 -1。此前这里用
        // GetCaretPos 兜底 —— 但系统 caret 已被 HideCaret 压住，该函数返回的
        // 是「线程 caret」的位置，往往是 (0,0)，蓝线就被画到了文本开头，
        // 用户看到的正是「光标跑到最前面」。改为用前一个字符的位置 + 该字符
        // 实测宽度推算插入点，纯几何计算，不依赖任何全局状态
        LRESULT q = SendMessageW(h, EM_POSFROMCHAR, (WPARAM)(int)(s0 - 1), 0);
        if (q != -1) {
            x = (int)(short)LOWORD(q);
            y = (int)(short)HIWORD(q);
            int adv = 0;
            std::wstring all;
            int n = GetWindowTextLengthW(h);
            if (n > 0) {
                all.resize((size_t)n + 1);
                GetWindowTextW(h, &all[0], n + 1);
                all.resize(wcslen(all.c_str()));
            }
            if ((int)s0 - 1 < (int)all.size()) {
                wchar_t ch[3] = { 0, 0, 0 };
                ch[0] = all[(size_t)s0 - 1];
                int cnt = 1;
                if (ch[0] >= 0xD800 && ch[0] <= 0xDBFF &&
                    (int)s0 < (int)all.size()) {
                    ch[1] = all[(size_t)s0];
                    cnt = 2;                 // 代理对：整个字符一起测
                }
                HDC hdx = GetDC(h);
                HFONT fx = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
                HGDIOBJ ofx = fx ? SelectObject(hdx, fx) : nullptr;
                SIZE szx {};
                if (GetTextExtentPoint32W(hdx, ch, cnt, &szx)) adv = (int)szx.cx;
                if (ofx) SelectObject(hdx, ofx);
                ReleaseDC(h, hdx);
            }
            x += adv;
            located = true;
        }
    }
    if (!located && s0 == 0) {
        // 首字符且定位失败：格式矩形左上角就是插入点（CenterInput 设过的内边距）
        RECT fr {};
        if (SendMessageW(h, EM_GETRECT, 0, (LPARAM)&fr)) {
            x = fr.left;
            y = fr.top;
            located = true;
        }
    }
    if (!located) return none;           // 宁可这帧不画，也不能画错位置
    RECT crc {};
    if (!GetClientRect(h, &crc) || crc.bottom <= 0 || crc.right <= 0) return none;
    HDC hdc = GetDC(h);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ of = f ? SelectObject(hdc, f) : nullptr;
    TEXTMETRICW tm {};
    GetTextMetricsW(hdc, &tm);
    if (of) SelectObject(hdc, of);
    ReleaseDC(h, hdc);
    int lh = (tm.tmHeight > 0) ? (int)tm.tmHeight : Scale(16);
    int w  = Scale(2);
    if (w < 1) w = 1;
    if (x < crc.left || x >= crc.right)  return none;   // 光标不在可视区
    if (y < crc.top  || y >= crc.bottom) return none;
    int bottom = y + lh;
    if (bottom > crc.bottom) bottom = crc.bottom;
    return RECT{ x, y, x + w, bottom };
}

// v1.13.1：HideCaret 是「每调用一次加一」的隐藏计数，ShowCaret 才能减一。
// 若自绘定位失败要退回系统光标，必须把压进去的次数如数还回去，否则用户会
// 彻底看不到插入点。这里自行记账，保证可逆。
static int  g_caretHideN    = 0;      // 已压入的系统光标隐藏计数
static bool g_caretFallback = false;  // 自绘不可用：交还系统光标，不再接管
static int  g_caretMiss     = 0;      // 连续定位失败帧数

static void HideSystemCaret(HWND h) {
    if (!h || g_caretFallback) return;
    // v1.13.15：HideCaret 是「每调一次 +1」的计数，而 WM_PAINT 每帧都会走这里，
    // 长时间运行后计数会涨到几万，Detach 时逐个 ShowCaret 会卡住界面。压到
    // 足够深度（8 层，EDIT 自己最多也就 Show 一两次）后就不再累加
    if (g_caretHideN >= 8) return;
    HideCaret(h);
    ++g_caretHideN;
}

static void RestoreSystemCaret(HWND h) {
    if (!h) return;
    while (g_caretHideN > 0) { ShowCaret(h); --g_caretHideN; }
}

// v1.13.2：只失效竖线所在的窄条（旧位置 + 新位置）。此前每一帧都整框
// InvalidateRect，EDIT 会把整段文本重打一遍，闪烁本身就成了持续的小抖动。
static RECT g_caretLast { 0, 0, 0, 0 };

static void CaretInvalidate(HWND h, const RECT& next) {
    if (!h || !IsWindow(h)) return;
    // v1.13.15：g_caretLast 只表示「屏幕上真实存在的那条线」，由 PaintBlueCaret
    // 在画完之后写入。此前这里提前把它改成「将要画的位置」，一旦两帧之间没有
    // WM_PAINT，屏幕上那条更早的线就再也没人失效 —— 于是列表里留下一条永不
    // 消失的蓝线（这正是「光标跑到最前面」的另一半原因）
    if (g_caretLast.bottom > g_caretLast.top) {
        RECT a = g_caretLast;
        InflateRect(&a, Scale(3), Scale(2));
        InvalidateRect(h, &a, FALSE);
    }
    if (next.bottom > next.top) {
        RECT b = next;
        InflateRect(&b, Scale(3), Scale(2));
        InvalidateRect(h, &b, FALSE);
    }
}

// 在当前闪烁相位下把蓝色竖线画到编辑框表面（由子类过程的 WM_PAINT 末尾调用，
// 此时默认过程已画完文字，线条不会被覆盖）
static void PaintBlueCaret(HWND h) {
    if (!g_caretOn || g_caretHwnd != h) return;
    // v1.13.15：输入法组词期间不画自绘竖线 —— 此时 EDIT 正在绘制组合串
    //（带下划线），插入点由系统接管，自绘线补进去只会打架
    if (h == g_input && g_imeComposing) return;
    // v1.13.18：行内编辑框组词期间同理 —— 此时 EDIT 正在绘制带下划线的组合串
    if (h == g_rowEdit && g_rowImeComposing) return;
    RECT r = BlueCaretRect(h);
    if (r.right <= r.left || r.bottom <= r.top) {
        // 定位不出来（极端字号 / 光标滚出可视区）：连续 3 帧就退回系统光标，
        // 宁可显示一根默认黑线，也不能让用户完全看不到插入点
        if (!g_caretFallback && ++g_caretMiss >= 3) {
            g_caretFallback = true;
            RestoreSystemCaret(h);
        }
        return;
    }
    g_caretMiss = 0;
    HDC hdc = GetDC(h);
    if (hdc) {
        FillRect(hdc, &r, CachedBrush(kCaretCol));
        ReleaseDC(h, hdc);
    }
    g_caretLast = r;      // v1.13.15：记录「已经画在屏幕上的位置」，下一帧据此擦除
}

static void AttachBlueCaret(HWND h) {
    if (!h) return;
    if (g_caretHwnd && g_caretHwnd != h) DetachBlueCaret();
    g_caretHwnd     = h;
    g_caretOn       = true;
    g_caretFallback = false;
    g_caretMiss     = 0;
    g_caretHideN    = 0;
    HideSystemCaret(h);                  // 系统黑光标不再出现
    g_caretLast = RECT{ 0, 0, 0, 0 };
    if (g_caretTimer) KillTimer(h, 3);
    g_caretTimer = SetTimer(h, 3, 530, nullptr);
    InvalidateRect(h, nullptr, TRUE);    // 首次接管：整框一次，建立干净底
}

static void DetachBlueCaret() {
    HWND h = g_caretHwnd;
    g_caretHwnd = nullptr;
    if (h && IsWindow(h)) {
        if (g_caretTimer) KillTimer(h, 3);
        RestoreSystemCaret(h);              // 交还系统光标
        g_caretFallback = false;
        CaretInvalidate(h, RECT{ 0, 0, 0, 0 });   // 只擦竖线所在窄条
        g_caretLast = RECT{ 0, 0, 0, 0 };
    }
    g_caretTimer = 0;
}

// 编辑框内容 / 光标位置变化后重置相位为「可见」，并整框重绘（旧线随之清除）
static void CaretKick(HWND h) {
    if (!h || g_caretHwnd != h || !IsWindow(h)) return;
    g_caretOn = true;
    HideSystemCaret(h);
    if (g_caretTimer) KillTimer(h, 3);
    g_caretTimer = SetTimer(h, 3, 530, nullptr);
    CaretInvalidate(h, BlueCaretRect(h));   // 插入点可能变了：新旧窄条一起失效
}

// 闪烁定时器：翻转相位并整框重绘（WM_PAINT 里按相位决定是否落笔）
static void CaretBlink(HWND h) {
    if (!h || g_caretHwnd != h) return;
    g_caretOn = !g_caretOn;
    HideSystemCaret(h);
    CaretInvalidate(h, BlueCaretRect(h));
}

// v1.13.3：让编辑中的插入点留在列表视口内 —— 已展开的长任务行高可能超过
// 视口，光标移到视口外时列表跟着滚（只改滚动位置、不动行高；编辑框由
// RepositionRowEditor 跟随所在行一起位移）
static void EnsureCaretInList() {
    if (!g_rowEdit || g_rowEditRow < 0 || g_rowEditRow >= (int)g_rows.size()) return;
    RECT cr = BlueCaretRect(g_rowEdit);          // 编辑框客户区坐标
    if (cr.bottom <= cr.top) return;
    RECT ir = RowRect(g_rowEditRow);
    int top = ir.top + Scale(10) + (int)cr.top;  // 编辑框顶 = 行顶 + Scale(10)
    int bot = ir.top + Scale(10) + (int)cr.bottom;
    int margin = Scale(12);
    if (top < margin)
        ListScrollTo(g_scrollPx + top - margin, false);
    else if (bot > g_viewH - margin)
        ListScrollTo(g_scrollPx + bot - (g_viewH - margin), false);
}

// v0.17：行内编辑框矩形（OpenRowEditor 与编辑中尺寸同步共用一份几何）：
//   · 单行内容：紧凑高度（≥ 行高 + 余量），在行内垂直居中（旧外观不变）
//   · 多行内容：与显示态文本区对齐（上下内边距 Scale(10)，占满行内容区）
// v0.18：编辑框高度总是略大于内容所需 —— EDIT 控件在光标落到末行时会
//   检查光标是否「完全可见」，高度恰好等于 N × 行高时光标底部可能越界
//   1~2px，触发整个内容向上滚动一行、第一行被顶出；多行余量由行高公式
//   （TaskRowHeight 编辑态 +Scale(4)）提供，单行紧凑框在这里同样预留，
//   大字号任务（行高超过 Scale(22)）也不再内滚
static RECT RowEditBoxRect(int row, int lines) {
    if (row < 0 || row >= (int)g_rows.size() || g_rows[row] < 0)
        return RECT{ 0, 0, 0, 0 };
    Task& t = g_tasks[(size_t)g_rows[row]];
    int whenW = WhenLabelWidth(t);        // 文本区右端避开日期标签
    RECT ir = RowRect(row);
    int left  = ir.left + Scale(12) + Scale(18) + Scale(12);
    int right = ir.right - Scale(12) - RowGutter() - (whenW ? whenW + Scale(12) : 0);
    if (right - left < Scale(80)) right = left + Scale(80);
    HDC hdc = GetDC(g_list);
    HGDIOBJ of = SelectObject(hdc, t.done ? g_fontDone : g_font);
    int lh = TaskLineHeight(hdc);
    SelectObject(hdc, of);
    ReleaseDC(g_list, hdc);
    if (lines <= 1) {
        // v0.27.1：编辑框窗口顶边 = 显示态单行文本的行矩形顶边（与
        // DrawTaskRow 单行分支同公式：行顶 + (行高-lineH)/2），3px 光标
        // 余量全部留在底部 —— 两态字形同机制同矩形，双击进入编辑零跳动
        //（v0.18 的“末行光标可见”余量保留，仅不再居中分配）
        int bh = lh + Scale(3);
        if (bh < Scale(22)) bh = Scale(22);
        int ty = ir.top + ((ir.bottom - ir.top) - lh) / 2;
        if (ty < ir.top) ty = ir.top;
        if (ty + bh > ir.bottom) bh = ir.bottom - ty;   // 极大字号不溢出行
        if (bh < lh) bh = lh;
        return RECT{ left, ty, right, ty + bh };
    }
    int eh  = (ir.bottom - ir.top) - Scale(20);   // 行高已含 v0.18 的 4px 余量
    if (eh < lh + Scale(3)) eh = lh + Scale(3);
    int top = ir.top + Scale(10);
    return RECT{ left, top, right, top + eh };
}

// v0.18：内容整放得下时，垂直滚动只可能是 EDIT 把末行光标误判为
//「不可见」时的自作主张 —— 首可见行一旦偏离 0 立即滚回顶部，保证
// 第一行永不被顶出视野。
// v0.23：内容超过视口容量（RowEditMaxLines 封顶）时内滚是正当行为，
// 不再钉回 —— 滚动条与滚轮都交给 EDIT 自己管理
static void KeepRowEditFullyVisible(HWND ed) {
    if (!ed) return;
    if (RowEditMaxLines(g_rowEditLines) > 0) return;   // v0.23：封顶模式内滚合法
    int first = (int)SendMessageW(ed, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (first > 0) SendMessageW(ed, EM_LINESCROLL, 0, -first);
}

// v0.23：按内容行数同步编辑框滚动条 —— 内容超高封顶时显示 WS_VSCROLL，
// 放得下时隐藏（保持无滚动条的紧凑外观）。滚动条占宽会改变折行结果，
// 调用方需在样式变化后重新实测行数
static void SyncRowEditScroll(HWND ed, int lines) {
    if (!ed) return;
    bool need = RowEditMaxLines(lines) > 0;
    bool has  = (GetWindowLongPtrW(ed, GWL_STYLE) & WS_VSCROLL) != 0;
    if (need == has) return;
    LONG_PTR st = GetWindowLongPtrW(ed, GWL_STYLE);
    SetWindowLongPtrW(ed, GWL_STYLE,
                      need ? (st | WS_VSCROLL) : (st & ~WS_VSCROLL));
    SetWindowPos(ed, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
}

// v0.17：把行内编辑框移动 / 缩放到其所在行的当前位置（滚动帧与行高
// 变化后调用，编辑框始终与所在行重合）
// v1.13.3：列表滚动后若编辑框已完全滚出视口，立即保存并关闭 —— 滚轮现在
// 不再强制退出编辑，焦点不能留在一个看不见的控件上
static void KeepRowEditorInView() {
    if (!g_rowEdit || g_rowEditRow < 0) return;
    RECT ir = RowRect(g_rowEditRow);
    if (ir.bottom <= 0 || ir.top >= g_viewH) CloseRowEditor(true);
}

static void RepositionRowEditor() {
    if (!g_rowEdit || g_rowEditRow < 0) return;
    // v1.13.18：组词期间只摆位置、不动格式矩形与内滚位置。
    // MoveWindow 会让 EDIT 重排，EM_SETRECT / EM_LINESCROLL 更是直接干预排版，
    // 三者都可能把正在提交的组合串的插入点顶回开头
    bool composing = g_rowImeComposing;
    RECT box = RowEditBoxRect(g_rowEditRow, g_rowEditLines);
    if (box.right <= box.left || box.bottom <= box.top) return;
    MoveWindow(g_rowEdit, box.left, box.top, box.right - box.left,
               box.bottom - box.top, TRUE);
    RECT er {}, er0 {};
    if (!composing && GetClientRect(g_rowEdit, &er) && er.right > 0) {
        SendMessageW(g_rowEdit, EM_GETRECT, 0, (LPARAM)&er0);
        if (er0.left != er.left || er0.top != er.top ||
            er0.right != er.right || er0.bottom != er.bottom)
            SendMessageW(g_rowEdit, EM_SETRECT, 0, (LPARAM)&er);
        // v0.23：格式矩形没变就不重设 —— EM_SETRECT 可能重置内滚位置，
        // 封顶模式下会打断用户正在进行的滚动
    }
    if (!composing)
        KeepRowEditFullyVisible(g_rowEdit);   // v0.18：任何重摆后都回正内滚
}

// v0.17：编辑框内容行数变化（Shift+Enter 换行 / 删除换行 / 粘贴多行 /
// 撤销）时，行高与编辑框高度同步生长或回缩 —— 单行任务也能在行内编辑时换行；
// v0.23：滚动条随封顶状态同步（内容增删跨越视口容量时出现/消失），
// 滚动条占宽会改变折行，行数需重新实测（有界循环防抖）
static void SyncRowEditorSize() {
    if (!g_rowEdit || g_rowEditRow < 0) return;
    // v1.13.18：输入法组词期间完全冻结重排。组词串（未上屏的候选文字）会
    // 让 EM_GETLINECOUNT 在「单行↔多行」之间抖动，一旦据此执行下面的
    // MoveWindow + EM_SETRECT，EDIT 立刻重排，插入点被清掉 —— 中文输入
    // 「后天」正常、补「下午」时插入点跑到『后』前面，就是这里被触发的。
    // 上屏后文本变化会补发一次（RowEditProc 的 EN_CHANGE 路径 + 组词结束）
    if (g_rowImeComposing) return;
    bool changed = false;
    for (int pass = 0; pass < 4; ++pass) {
        int lines = (int)SendMessageW(g_rowEdit, EM_GETLINECOUNT, 0, 0);
        if (lines < 1) lines = 1;
        if (lines != g_rowEditLines) { g_rowEditLines = lines; changed = true; }
        bool hasV  = (GetWindowLongPtrW(g_rowEdit, GWL_STYLE) & WS_VSCROLL) != 0;
        bool needV = RowEditMaxLines(lines) > 0;
        if (needV == hasV) break;
        SyncRowEditScroll(g_rowEdit, lines);  // 样式变化 → 折行可能变化 → 重测
        changed = true;
    }
    if (!changed) return;                // 行数与滚动条状态都没变，无需重摆
    int idx0 = (g_rowEditRow >= 0 && g_rowEditRow < (int)g_rows.size())
                   ? g_rows[g_rowEditRow] : -1;
    int anchorY = RowAnchorY(idx0);      // v1.13.1：行顶原地不动
    RebuildRowTops();                    // 行高公式含编辑态覆盖，重算前缀和与填塞行
    ListRepaint();
    RestoreRowAnchor(idx0, anchorY);     // 替代 ListEnsureVisible（不再把行底硬滚进视口）
    RepositionRowEditor();
    if (RowEditMaxLines(g_rowEditLines) == 0)
        KeepRowEditFullyVisible(g_rowEdit);  // v0.18：放得下时回正内滚；v0.23 封顶时内滚合法
}

// v1.13.22：行内编辑框的「插入点守恒」，与底部添加框 PassKeepCaret 同规则。
// 只用于真正的文档插入（WM_CHAR / WM_PASTE）：基准取「处理本次消息前的选区
// 起点」，按新增字数推算插入点。IME 消息一律不进这个函数 —— 组词期间插入点
// 完全属于输入法，外部任何 EM_SETSEL 干预都会把光标拽回去。
static LRESULT RowPassKeepCaret(HWND h, UINT m, WPARAM w, LPARAM l) {
    DWORD b0 = 0, b1 = 0;
    CallWindowProcW(g_oldRowEditProc, h, EM_GETSEL, (WPARAM)&b0, (LPARAM)&b1);
    DWORD base = b0;
    int lenBefore = (int)CallWindowProcW(g_oldRowEditProc, h, WM_GETTEXTLENGTH, 0, 0);

    LRESULT r = CallWindowProcW(g_oldRowEditProc, h, m, w, l);

    int lenAfter = (int)CallWindowProcW(g_oldRowEditProc, h, WM_GETTEXTLENGTH, 0, 0);
    int added = lenAfter - lenBefore;
    if (added > 0) {
        DWORD want = base + (DWORD)added;
        if ((int)want > lenAfter) want = (DWORD)lenAfter;
        DWORD a0 = 0, a1 = 0;
        CallWindowProcW(g_oldRowEditProc, h, EM_GETSEL, (WPARAM)&a0, (LPARAM)&a1);
        if (a1 != want)
            CallWindowProcW(g_oldRowEditProc, h, EM_SETSEL, want, want);
    }
    return r;
}

// 行内编辑框过程：Enter 保存、Esc 取消、Tab/失焦保存；
// v0.16：多行编辑框中 Shift+Enter = 插入换行（交给默认过程）
static LRESULT CALLBACK RowEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // v1.13.22：与底部添加框同一套防护 —— 拦截外部（输入法上屏收尾）发来的
    // EM_SETSEL(0,0)，避免刚提交的文字把插入点带回行首。判定条件见 EditProc
    if (m == EM_SETSEL && (int)w == 0 && (int)l == 0) {
        DWORD r0 = 0, r1 = 0;
        CallWindowProcW(g_oldRowEditProc, h, EM_GETSEL, (WPARAM)&r0, (LPARAM)&r1);
        int lenNow = (int)CallWindowProcW(g_oldRowEditProc, h, WM_GETTEXTLENGTH, 0, 0);
        if (r1 > 0 && r1 == (DWORD)lenNow) {
            return 0;      // 拒绝把插入点带回收件箱行首
        }
    }
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ml    = (GetWindowLongPtrW(h, GWL_STYLE) & ES_MULTILINE) != 0;
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN && !(shift && ml)) { CloseRowEditor(true);  return 0; }
        if (w == VK_ESCAPE) { CloseRowEditor(false); return 0; }
        if (w == VK_TAB)    { CloseRowEditor(true);  return 0; }
    } else if (m == WM_CHAR && (w == L'\r' || w == L'\n')) {
        if (!(shift && ml)) return 0;
    } else if (m == WM_KILLFOCUS) {
        DetachBlueCaret();                 // v1.13.1：先交还光标，再关编辑框
        CloseRowEditor(true);
        return 0;
    } else if (m == WM_TIMER && w == 3) {  // v1.13.1：蓝色光标闪烁
        CaretBlink(h);
        return 0;
    } else if (m == WM_MOUSEWHEEL) {
        // v1.13.4：滚轮语义全应用统一 —— 折叠、展开、编辑，滚轮永远只滚列表。
        // 编辑框内的长文本用右侧滚动条，或用光标移动（方向键 / PgUp / Ctrl+End）
        // 让 EDIT 自己跟随；不再出现「指针在框内滚内容、在框外滚列表」两套手感
        return SendMessageW(g_list, WM_MOUSEWHEEL, w, l);
    }
    // v1.13.18：记录组词起止。START 时记下插入点作为提交后的基准；
    // END 只清标志并补一次重排 —— 此刻组合串刚提交、选区还在收尾，
    // 任何 MoveWindow / EM_SETRECT / EM_LINESCROLL 都会把插入点顶回开头
    if (m == WM_IME_STARTCOMPOSITION) {
        g_rowImeComposing = true;
    } else if (m == WM_IME_ENDCOMPOSITION) {
        g_rowImeComposing = false;
    }
    // v1.13.22：只对真正的文档插入做守恒，IME 消息交给原生 EDIT/输入法
    LRESULT r;
    if (m == WM_CHAR || m == WM_PASTE)
        r = RowPassKeepCaret(h, m, w, l);
    else
        r = CallWindowProcW(g_oldRowEditProc, h, m, w, l);
    // v1.13.18：组词刚上屏时同样不重排 —— 与底部添加框 v1.13.16 的处理一致。
    // 上屏后的文本变化会在下面这条 EN_CHANGE 投递路径里补算
    if (m == WM_IME_ENDCOMPOSITION) {
        PostMessageW(g_list, WM_ROWEDITCHK, 0, 0);
    }
    if (m == WM_PAINT) {                 // v1.13.1：EDIT 每帧都会重新 ShowCaret
        HideSystemCaret(h);              //  —— 逐帧压住系统黑光标，改画蓝线
        PaintBlueCaret(h);
    } else if (m == WM_SETFOCUS) {
        g_rowImeComposing = false;       // v1.13.18：输入法被强制取消时不会发 ENDCOMPOSITION
        AttachBlueCaret(h);
    }
    // v0.17：按键 / 粘贴 / 撤销后内容行数可能变化 —— 行高与编辑框同步；
    // v0.18：编辑框右键菜单的粘贴 / 撤销（WM_PASTE / WM_UNDO / WM_CLEAR）
    // 同样改变内容行数，一并同步
    if (m == WM_KEYDOWN || m == WM_CHAR ||
        m == WM_PASTE || m == WM_UNDO || m == WM_CLEAR)
        SyncRowEditorSize();              // v1.13.18：组词期间函数内部直接返回
    // v0.18：光标 / 选择变化（含单击、拖选、方向键）后 EDIT 可能把末行
    // 光标误判为不可见而内滚一行（第一行被顶出）—— 内容总是放得下时
    // 立即滚回顶部，编辑期间所有文本保持完整可见
    // v1.13.18：组词期间跳过 —— EM_LINESCROLL 属于对 EDIT 的排版干预
    if ((m == WM_KEYDOWN || m == WM_CHAR || m == WM_PASTE || m == WM_UNDO ||
         m == WM_CLEAR || m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK ||
         m == WM_MOUSEMOVE) && !g_rowImeComposing)
        KeepRowEditFullyVisible(h);
    // v1.13.1：插入点/内容变了 —— 蓝线立即回到可见相位并重绘
    if (m == WM_KEYDOWN || m == WM_CHAR || m == WM_PASTE || m == WM_UNDO ||
        m == WM_CLEAR || m == WM_SETTEXT || m == WM_LBUTTONDOWN ||
        m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK)
        CaretKick(h);
    if (m == WM_MOUSEMOVE && (GetKeyState(VK_LBUTTON) & 0x8000))
        CaretKick(h);                    // 拖选：线跟随新的插入点
    // v1.13.3：插入点跑出视口时列表跟着滚（长任务展开后行高 > 视口）
    // v1.13.18：组词期间跳过 —— 列表滚动会触发 RepositionRowEditor 的
    // MoveWindow，正在提交的组合串会被重排打断
    if ((m == WM_KEYDOWN || m == WM_CHAR || m == WM_LBUTTONDOWN ||
         m == WM_LBUTTONUP) && !g_rowImeComposing)
        EnsureCaretInList();
    return r;
}

// 在任务行上打开行内编辑框（clickPt 为列表客户区坐标，用于把光标放到点击处）
static void OpenRowEditor(int row, POINT clickPt) {
    if (row < 0 || row >= (int)g_rows.size() || g_rows[row] < 0) return;
    int idx0 = g_rows[row];
    CloseRowEditor(true);                    // 已有编辑框先保存
    int row2 = RowOfIdx(idx0);               // v1.13.1：保存可能重建行，重定位
    if (row2 < 0) return;
    row = row2;
    int idx = idx0;
    RECT ir = RowRect(row);                  // v1.13.0：自绘列表行矩形
    if (ir.bottom <= ir.top) return;
    // v1.13.5：锁定行顶的视口位置 —— 双击触发展开后，用户点的那一行不移动
    int anchorY = RowAnchorY(idx);
    // 文本区右端避开日期标签（与 DrawTaskRow 布局一致，含「＋ 日期」占位）
    Task& t = g_tasks[(size_t)idx];
    int whenW = WhenLabelWidth(t);
    int left  = ir.left + Scale(12) + Scale(18) + Scale(12);
    int right = ir.right - Scale(12) - RowGutter() - (whenW ? whenW + Scale(12) : 0);
    if (right - left < Scale(80)) right = left + Scale(80);
    // v1.12.1：是否多行按折行结果判定（无 \n 的长文本折行后同样用多行编辑框）；
    // 折行超过 5 行且当前折叠时，先展开任务再取行矩形（编辑框盖住全部行）
    HDC hdc = GetDC(g_list);
    HGDIOBJ of = SelectObject(hdc, t.done ? g_fontDone : g_font);
    std::vector<std::wstring> lines = WrapTaskText(hdc, t.text, right - left);
    SelectObject(hdc, of);
    ReleaseDC(g_list, hdc);
    int total = (int)lines.size();

    // v1.13.5：双击即展开到全文高度 —— 编辑态与展开态共用同一条行高公式，
    // 编辑框只是覆盖在整段文字上的一层，进出编辑行高逐像素不变。
    // 退出编辑（Esc / 点其他任务 / 点空白）只销毁编辑框：保持展开、
    // 继续显示「收起」按钮，即「展开选中态 + 一个光标」
    if (!t.expanded) {
        t.expanded = true;
        RefreshListKeepSel();
        row = RowOfIdx(idx);
        if (row < 0) return;
        ir = RowRect(row);
    }
    // v0.17：编辑框一律按多行创建（ES_MULTILINE + ES_AUTOVSCROLL +
    // ES_WANTRETURN）—— 单行任务双击编辑后同样能 Shift+Enter 换行，行高随
    // 内容行数生长（SyncRowEditorSize）；单行内容时保持紧凑居中的旧外观。
    // v0.25：去掉 WS_BORDER —— 灰色边框线条不再显示，编辑框与选中行底色融合
    DWORD edStyle = WS_CHILD | WS_VISIBLE | ES_LEFT |
                    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    g_rowEditRow   = row;
    g_rowEditIdx   = idx;
    g_rowEditLines = (total > 1) ? total : 1;
    // v0.23：内容超过视口容量时编辑框封顶并带滚动条（放得下保持无滚动条外观）
    if (RowEditMaxLines(g_rowEditLines) > 0) edStyle |= WS_VSCROLL;
    RECT box = RowEditBoxRect(row, g_rowEditLines);
    if (box.right <= box.left || box.bottom <= box.top) {
        g_rowEditRow   = -1;
        g_rowEditIdx   = -1;
        g_rowEditLines = 0;
        return;
    }
    // v0.22：空控件创建、设好字体与格式矩形后由 WM_SETTEXT 载入文本
    //（EDIT 只认 \r\n 为换行，纯 \n 会挤成一行）。此前 CreateWindow 直接
    // 载入文本再设字体/改格式矩形，控件的折行是懒重算的，随后的
    // EM_GETLINECOUNT 可能拿到按旧字体折行的过期结果
    g_rowEdit = CreateWindowExW(0, L"EDIT", nullptr, edStyle,
        box.left, box.top, box.right - box.left, box.bottom - box.top,
        g_list, (HMENU)(INT_PTR)IDC_ROWEDIT, GetModuleHandleW(nullptr), nullptr);
    if (!g_rowEdit) {
        g_rowEditRow   = -1;
        g_rowEditIdx   = -1;
        g_rowEditLines = 0;
        return;
    }
    SendMessageW(g_rowEdit, WM_SETFONT,
                 (WPARAM)(t.done ? g_fontDone : g_font), TRUE);
    // 清除 EDIT 默认内边距，编辑框内容区与显示态文本区完全对齐（不内滚）。
    // v0.27.1：单行编辑框窗口顶边已与显示态行矩形顶边同位（见
    // RowEditBoxRect 单行分支与 DrawTaskRow 单行分支：同一 ty 公式、
    // 同一顶对齐机制），格式矩形取满客户区即可两态字形同位，无需再做
    // 居中补偿；多行框同样满客户区（行顶对齐，与显示态多行绘制同位）
    RECT er {};
    GetClientRect(g_rowEdit, &er);
    SendMessageW(g_rowEdit, EM_SETRECT, 0, (LPARAM)&er);
    SendMessageW(g_rowEdit, WM_SETTEXT, 0, (LPARAM)ToCrLf(t.text).c_str());
    // v0.22：首次打开一律同步重摆 —— 行高按编辑态公式重算（含 4px 余量）
    // 并重摆编辑框。此前实测行数恰好等于预判时直接跳过，而首次打开算
    // 矩形时编辑框还不存在（TaskRowHeight 走显示态公式、无余量），编辑框
    // 高度恰好等于 行数×行高，末行光标底部越界 1~2px 被裁（又被「首可见
    // 行钉回顶部」的保护拉回），光标点到最后一行最后位置始终看不见；
    // 按键后 SyncRowEditorSize 才用编辑态公式补上余量（第二次编辑才正常）
    int realLines = (int)SendMessageW(g_rowEdit, EM_GETLINECOUNT, 0, 0);
    if (realLines > 0 && realLines != g_rowEditLines)
        g_rowEditLines = realLines;   // 控件实测优先（折行差 1 行场景，v0.20）
    SyncRowEditScroll(g_rowEdit, g_rowEditLines);   // v0.23：按实测行数复核滚动条
    RebuildRowTops();
    ListRepaint();
    RestoreRowAnchor(idx, anchorY);   // v1.13.5：行顶原地不动（展开后仍停在原处）
    RepositionRowEditor();
    KeepRowEditFullyVisible(g_rowEdit);
    g_oldRowEditProc = (WNDPROC)SetWindowLongPtrW(g_rowEdit, GWLP_WNDPROC,
                                                  (LONG_PTR)RowEditProc);
    // v0.20：先给焦点再置光标 —— 有焦点时 EM_SETSEL 才会创建并正确定位
    // caret；无焦点时置末尾、SetFocus 后多行场景光标可能不显示
    SetFocus(g_rowEdit);
    int len = GetWindowTextLengthW(g_rowEdit);
    // v1.13.8：光标落到「双击那一行的行尾」，而不是行首、也不是点击处 ——
    // 任务文本是逐行写的一条条内容，双击的意图基本都是在这一行后面续写；
    // 落在行首会把新字符插到行号/开头文字之前，落在行中间则要先把光标挪到
    // 末尾才能继续打。这里先定位到点击所在的行，再取该行末尾
    int caret = len;
    if (clickPt.x >= 0 && clickPt.y >= 0) {
        POINT cp = { clickPt.x - box.left, clickPt.y - box.top };
        // 点击常落在行的上下内边距（文本矩形之外），EM_CHARFROMPOS 会返回 -1；
        // 先钳进客户区，点行顶留白算首行、点行底留白算末行
        RECT ec {};
        GetClientRect(g_rowEdit, &ec);
        if (cp.x < ec.left) cp.x = ec.left;
        if (cp.y < ec.top)  cp.y = ec.top;
        if (cp.x > ec.right  - 1) cp.x = ec.right  - 1;
        if (cp.y > ec.bottom - 1) cp.y = ec.bottom - 1;

        int line = -1;
        LRESULT ci = SendMessageW(g_rowEdit, EM_CHARFROMPOS, 0, (LPARAM)&cp);
        int ciIdx = (int)(short)LOWORD(ci);
        if (ci >= 0 && ciIdx >= 0 && ciIdx <= len)
            line = (int)SendMessageW(g_rowEdit, EM_LINEFROMCHAR, (WPARAM)ciIdx, 0);
        if (line < 0) {
            // 兜底：按编辑框行高反推行号
            TEXTMETRICW tm2 {};
            HDC hdc2 = GetDC(g_rowEdit);
            HFONT f2 = (HFONT)SendMessageW(g_rowEdit, WM_GETFONT, 0, 0);
            HGDIOBJ of2 = f2 ? SelectObject(hdc2, f2) : nullptr;
            GetTextMetricsW(hdc2, &tm2);
            if (of2) SelectObject(hdc2, of2);
            ReleaseDC(g_rowEdit, hdc2);
            int lh2 = (tm2.tmHeight > 0)
                          ? (int)tm2.tmHeight + (int)tm2.tmExternalLeading
                          : Scale(16);
            line = (lh2 > 0 && cp.y > 0) ? (cp.y / lh2) : 0;
        }
        if (line >= 0) {
            LRESULT ls = SendMessageW(g_rowEdit, EM_LINEINDEX, (WPARAM)line, 0);
            if ((int)ls >= 0 && (int)ls <= len) {
                int lineLen = (int)SendMessageW(g_rowEdit, EM_LINELENGTH,
                                                 (WPARAM)(int)ls, 0);
                if (lineLen < 0) lineLen = 0;
                caret = (int)ls + lineLen;      // 该行行尾
                if (caret > len) caret = len;
            }
        }
    }
    SendMessageW(g_rowEdit, EM_SETSEL, caret, caret);
    // v0.23：封顶模式下框只显示部分内容，滚到光标处（内容末端）让光标
    // 可见；未封顶时内容整篇铺开，保持 v0.22「首行在顶」的行为
    if (RowEditMaxLines(g_rowEditLines) > 0)
        SendMessageW(g_rowEdit, EM_SCROLLCARET, 0, 0);
    // v0.18/v0.22：光标落到末行时 EDIT 可能误滚动一行（第一行被顶出），
    // 立即回正；置选/焦点可能触发控件重算折行 —— 再复核一次行数
    int afterLines = (int)SendMessageW(g_rowEdit, EM_GETLINECOUNT, 0, 0);
    if (afterLines > 0 && afterLines != g_rowEditLines) SyncRowEditorSize();
    else KeepRowEditFullyVisible(g_rowEdit);
    // v0.23：封顶模式下重摆/复核可能重置内滚位置 —— 最后再滚到光标一次
    if (RowEditMaxLines(g_rowEditLines) > 0)
        SendMessageW(g_rowEdit, EM_SCROLLCARET, 0, 0);
    // v1.13.3：编辑框可能比视口高（已展开的长任务），把光标滚进视口
    EnsureCaretInList();
    // v0.22：首帧绘制后再异步复核一次行数（列表过程收 WM_ROWEDITCHK），
    // 兜底任何残存的懒折行时序 —— 任何路径下末行光标都完整可见
    PostMessageW(g_list, WM_ROWEDITCHK, 0, 0);
}

// 删除指定显示行对应的任务（右键菜单用，待办/已完成均可）
static void DeleteTaskAt(int row) {
    if (row < 0 || row >= (int)g_rows.size() || g_rows[row] < 0) return;
    int idx = g_rows[row];
    if (g_rowEditRow == row) CloseRowEditor(false);  // 正在编辑则先关编辑框
    g_tasks.erase(g_tasks.begin() + idx);
    SaveTasks();
    RebuildRows();
    UpdateButtons();
}

static void UpdateInputHint() {
    int n = GetWindowTextLengthW(g_input);
    std::wstring raw;
    raw.resize((size_t)n + 1);
    GetWindowTextW(g_input, &raw[0], n + 1);
    raw.resize(wcslen(raw.c_str()));
    if (raw.find(L'\n') != std::wstring::npos) {
        // v1.12.0 多行粘贴：提示整段添加
        int lines = (int)SendMessageW(g_input, EM_GETLINECOUNT, 0, 0);
        if (lines < 1) lines = 1;
        g_hintText = L"多行文本 " + std::to_wstring(lines) +
                     L" 行 · 回车整段添加，Shift+Enter 换行";
    } else {
        WhenResult w = ParseWhen(raw);
        if (w.ok) {
            std::wstring when = FormatWhen(w.due, w.hasTime, false, nullptr);
            if (!when.empty())
                g_hintText = L"识别到：" + when + L"（回车添加，自动归类到任务）";
            else
                g_hintText.clear();
        } else {
            g_hintText.clear();
        }
    }
    InvalidateRect(g_hwnd, &g_hintRect, TRUE);
}

// v1.12.0：输入框随内容行数长高（粘贴多行文本时），并随 Layout 重新排布
static int g_inputH = 0;   // 0 = 尚未按内容计算（Layout 按 1 行高度处理）
static void UpdateInputSize() {
    if (!g_input || !g_hwnd) return;
    // v1.13.17：输入法组词期间不做任何布局 —— 组词串会让 EM_GETLINECOUNT 抖动
    // （单行 ↔ 双行），一旦据此走 Layout，就会 MoveWindow 改输入框尺寸，
    // 编辑框重排、插入点被清掉。上屏后文本变化会再发 EN_CHANGE，届时补算
    if (g_imeComposing) return;
    int lines = (int)SendMessageW(g_input, EM_GETLINECOUNT, 0, 0);
    if (lines < 1) lines = 1;
    int h = (lines <= 1) ? Scale(36)
          : Scale(24) + ((lines > 4) ? 4 : lines) * Scale(22);
    if (h != g_inputH) {
        g_inputH = h;
        Layout();                        // Layout 内部已调 CenterInput
        InvalidateRect(g_hwnd, nullptr, TRUE);
    }
    // v1.13.16：行数没变就什么都不做 —— 格式矩形只取决于窗口尺寸（Layout 负责）
    // 与字体（ApplyFonts 负责），这里没有需要重算的东西。此前无条件调
    // CenterInput，让「每敲一个字都去动一次 EDIT 的格式矩形」，是输入时插入点
    // 被打断的总根源
}

static void AddFromInput() {
    int n = GetWindowTextLengthW(g_input);
    std::wstring raw;
    raw.resize((size_t)n + 1);
    GetWindowTextW(g_input, &raw[0], n + 1);
    raw.resize(wcslen(raw.c_str()));
    std::wstring text = NormalizeMultiline(raw);   // v1.12.0：保留换行结构
    if (text.empty()) {
        MessageBeep(MB_ICONWARNING);
        SetFocus(g_input);
        return;
    }
    Task t;
    if (text.find(L'\n') == std::wstring::npos) {
        WhenResult w = ParseWhen(text);
        if (w.ok && !w.stripped.empty()) {
            t.text    = w.stripped;
            t.due     = w.due;
            t.hasTime = w.hasTime;
        } else {
            t.text = text;   // 日期词占满整行（如只输入“明天”）-> 按普通任务处理
        }
    } else {
        t.text = text;       // v1.12.0 多行文本：整段即任务，不做日期解析
    }
    g_tasks.insert(g_tasks.begin(), t);   // 新任务置顶
    g_multiSel.clear();                   // v0.16：下标整体 +1，多选失效
    SaveTasks();
    RebuildRows();
    g_selRow = -1;                        // 默认不选中
    g_anchorRow = -1;                     // v0.16：锚点一并重置
    SetWindowTextW(g_input, L"");
    SetFocus(g_input);
    UpdateButtons();
    UpdateInputSize();                    // 输入框回落到单行高度
    CheckReminders();                     // 新任务若已进入提醒窗口则立即提示
}

static void ToggleTask(int row) {
    if (row < 0 || row >= (int)g_rows.size()) return;
    int idx = g_rows[row];
    if (idx == ROW_FILL) return;             // v1.10.0 填塞行无操作
    if (idx < 0) { ToggleGroup(-1 - idx); return; }
    g_tasks[(size_t)idx].done = !g_tasks[(size_t)idx].done;
    if (g_tasks[(size_t)idx].done) {
        g_tasks[(size_t)idx].doneAt = (long long)time(nullptr);
    } else {
        // 取消完成 -> 移回待办区顶部
        Task t = g_tasks[(size_t)idx];
        t.done = false;
        t.doneAt = 0;
        t.notified = false;   // 重新进入提醒流程
        g_tasks.erase(g_tasks.begin() + idx);
        g_tasks.insert(g_tasks.begin(), t);
        g_multiSel.clear();               // v0.16：下标整体 +1，多选失效
        RebuildRows();
        g_selRow = 0;
        ListEnsureVisible(0);
        SaveTasks();
        UpdateButtons();
        return;
    }
    SaveTasks();
    RebuildRows();
    // 选中它在新分组中的行
    g_selRow = RowOfIdx(idx);
    if (g_selRow >= 0) ListEnsureVisible(g_selRow);
    ListRepaint();
    UpdateButtons();
}

// v0.16：批量完成 / 还原所选任务（还原的任务移回待办区顶部，保持原相对顺序）
static void MultiSetDone(const std::vector<int>& idxs, bool done) {
    if (idxs.empty()) return;
    std::vector<int> ids = idxs;
    std::sort(ids.begin(), ids.end());
    if (!done) {
        std::vector<Task> moved;
        for (int i : ids) {
            if (i < 0 || i >= (int)g_tasks.size()) continue;
            Task t = g_tasks[(size_t)i];
            t.done     = false;
            t.doneAt   = 0;
            t.notified = false;              // 重新进入提醒流程
            moved.push_back(t);
        }
        for (size_t k = ids.size(); k > 0; --k) {
            int i = ids[k - 1];
            if (i >= 0 && i < (int)g_tasks.size())
                g_tasks.erase(g_tasks.begin() + i);
        }
        for (size_t k = moved.size(); k > 0; --k)
            g_tasks.insert(g_tasks.begin(), moved[k - 1]);
    } else {
        long long now = (long long)time(nullptr);
        for (int i : ids)
            if (i >= 0 && i < (int)g_tasks.size()) {
                g_tasks[(size_t)i].done   = true;
                g_tasks[(size_t)i].doneAt = now;
            }
    }
    g_multiSel.clear();
    g_selRow    = -1;
    g_anchorRow = -1;
    SaveTasks();
    RebuildRows();
    UpdateButtons();
    ListRepaint();
}

// v0.16：批量删除所选任务
static void MultiDelete(const std::vector<int>& idxs) {
    if (idxs.empty()) return;
    std::vector<int> ids = idxs;
    std::sort(ids.begin(), ids.end(), [](int a, int b) { return a > b; });
    for (int i : ids)
        if (i >= 0 && i < (int)g_tasks.size())
            g_tasks.erase(g_tasks.begin() + i);
    g_multiSel.clear();
    g_selRow    = -1;
    g_anchorRow = -1;
    SaveTasks();
    RebuildRows();
    UpdateButtons();
    ListRepaint();
}

static void DeleteSelected() {
    if (!g_multiSel.empty()) {               // v0.16：多选时批量删除
        MultiDelete(g_multiSel);
        return;
    }
    if (g_selRow < 0 || g_selRow >= (int)g_rows.size() || g_rows[g_selRow] < 0) return;
    int idx = g_rows[g_selRow];
    g_tasks.erase(g_tasks.begin() + idx);
    SaveTasks();
    RebuildRows();
    UpdateButtons();
}

static void ClearDone() {
    for (int i = (int)g_tasks.size() - 1; i >= 0; --i) {
        if (g_tasks[(size_t)i].done) g_tasks.erase(g_tasks.begin() + i);
    }
    SaveTasks();
    RebuildRows();
    UpdateButtons();
}

// ---------------------------- 任务栏托盘 -------------------------------------
static void AddTrayIcon() {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                  IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!nid.hIcon)
        nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                                      MAKEINTRESOURCEW(1), IMAGE_ICON,
                                      16, 16, LR_DEFAULTCOLOR);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wchar_t tip[128] = L"";
    GetWindowTextW(g_hwnd, tip, 128);
    lstrcpynW(nid.szTip, tip, 128);
    g_nidAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

// NIM_ADD 偶发失败（任务栏未就绪等）：定时重试最多 5 次 x 0.5 秒
static void EnsureTrayIcon(HWND hwnd) {
    if (g_nidAdded) return;
    AddTrayIcon();
    if (!g_nidAdded && hwnd && g_trayAddTries++ < 5)
        SetTimer(hwnd, 4, 500, nullptr);
}

static void RemoveTrayIcon() {
    if (!g_nidAdded) return;
    NOTIFYICONDATAW nid {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_nidAdded = false;
}

// v1.9.0 心跳探测：图标真的还在吗？
// NIM_MODIFY 一个仍然存在的图标返回 TRUE；图标已被系统移除/未添加则返回
// FALSE。这里只重设 callback 消息（值不变），对显示零副作用，代价可忽略。
static bool TrayIconAlive() {
    if (!g_hwnd) return false;
    NOTIFYICONDATAW nid {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

// 提升后进入验证期：由心跳确认 IsPromoted=1 没有造成图标丢失
static void BeginTrayAliveCheck() {
    g_trayAliveCheck = true;
    g_aliveOk        = 0;
    g_aliveMiss      = 0;
}

// 把本程序匹配的所有 NotifyIconSettings 子键 IsPromoted 置为指定值，
// 返回是否找到匹配项（value=0 即回滚为系统默认：图标收进「^」折叠区）
static bool WriteIsPromoted(DWORD value) {
    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    CharUpperBuffW(exe, (DWORD)lstrlenW(exe));
    PCWSTR exeName = wcsrchr(exe, L'\\');
    exeName = exeName ? exeName + 1 : exe;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings",
                      0, KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t name[64];
    for (DWORD idx = 0;; ++idx) {
        DWORD len = 64;
        if (RegEnumKeyExW(root, idx, name, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) break;
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k)
            != ERROR_SUCCESS) continue;
        wchar_t raw[MAX_PATH * 2] = L"";
        DWORD sz = sizeof(raw), type = 0;
        if (RegQueryValueExW(k, L"ExecutablePath", nullptr, &type, (LPBYTE)raw,
                             &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            // 与 PromoteTrayIcon 相同的路径归一化
            std::wstring p = raw;
            if (p.compare(0, 4, L"\\??\\") == 0) p.erase(0, 4);
            if (!p.empty() && p.front() == L'"') p.erase(0, 1);
            if (!p.empty() && p.back()  == L'"') p.pop_back();
            wchar_t exp[MAX_PATH * 2] = L"";
            if (!ExpandEnvironmentStringsW(p.c_str(), exp, MAX_PATH * 2))
                lstrcpynW(exp, p.c_str(), MAX_PATH * 2);
            CharUpperBuffW(exp, (DWORD)lstrlenW(exp));
            bool same = !lstrcmpW(exp, exe);
            if (!same) {
                size_t n = lstrlenW(exe), m = lstrlenW(exp);
                same = (m > n && lstrcmpW(exp + (m - n), exe) == 0);
            }
            if (!same) {
                PCWSTR en = wcsrchr(exp, L'\\');
                en = en ? en + 1 : exp;
                same = !lstrcmpW(en, exeName);
            }
            if (same) {
                RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD,
                               (const BYTE*)&value, sizeof(value));
                found = true;
            }
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return found;
}

// 提升验证失败后的回滚：IsPromoted 置 0 并重加图标。
// 宁可让图标被收进「^」折叠区，也绝不让图标消失 —— 图标必在是底线
static void RollbackTrayPromotion() {
    g_trayPromoted = false;
    if (WriteIsPromoted(0)) {
        RemoveTrayIcon();
        AddTrayIcon();
    }
}

static void ShowMainWindow() {
    if (IsIconic(g_hwnd))              ShowWindow(g_hwnd, SW_RESTORE);
    else if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

static void HideToTray() {
    ShowWindow(g_hwnd, SW_HIDE);
    if (!g_trayTipShown) {               // 首次最小化提示一次
        g_trayTipShown = true;
        NOTIFYICONDATAW nid {};
        nid.cbSize      = sizeof(nid);
        nid.hWnd        = g_hwnd;
        nid.uID         = 1;
        nid.uFlags      = NIF_INFO;
        nid.uTimeout    = 3000;
        nid.dwInfoFlags = NIIF_INFO;
        lstrcpynW(nid.szInfoTitle, L"Tickly", 64);
        // 图标已提升为常驻时直接告知位置；未能提升（Win10 / 设置项未就绪）
        // 则引导用户去「^」折叠区找，避免找不到入口
        lstrcpynW(nid.szInfo,
                  g_trayPromoted
                      ? L"已关闭到屏幕右下角托盘区，到点提醒仍会弹出；点击托盘小图标可重新打开"
                      : L"已关闭到后台继续运行，到点提醒仍会弹出；若右下角未见图标，请点开任务栏「^」查看",
                  256);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

// 托盘小图标常驻任务栏右下角（类似微信/QQ）：
// Windows 默认会把新程序的托盘图标收进右下角「^」折叠区，导致关闭到后台后
// 找不到入口。这里只改本程序自己的 NotifyIconSettings 项（按 exe 路径匹配），
// 置 IsPromoted=1 让图标直接常驻任务栏角落，不碰其他程序的任何设置。
// PromoteTrayIcon 的结果
enum { TRAY_PROMOTE_NONE    = 0,   // 未找到本程序的设置项（Win10 无此项 / 子键未建好）
       TRAY_PROMOTE_WROTE   = 1,   // 本次写入了 IsPromoted=1
       TRAY_PROMOTE_ALREADY = 2 }; // 本程序图标已是常驻状态

static int PromoteTrayIcon() {
    // 注意：不加 g_nidAdded 守卫 —— 启动时需要“先预写、后加图标”的顺序
    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    CharUpperBuffW(exe, (DWORD)lstrlenW(exe));
    PCWSTR exeName = wcsrchr(exe, L'\\');
    exeName = exeName ? exeName + 1 : exe;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings",
                      0, KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS)
        return TRAY_PROMOTE_NONE;    // Win10 没有此项：无法编程提升，交给气泡文案引导
    int result = TRAY_PROMOTE_NONE;
    wchar_t name[64];
    for (DWORD idx = 0;; ++idx) {
        DWORD len = 64;
        if (RegEnumKeyExW(root, idx, name, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) break;
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k)
            != ERROR_SUCCESS) continue;
        wchar_t raw[MAX_PATH * 2] = L"";
        DWORD sz = sizeof(raw), type = 0;
        if (RegQueryValueExW(k, L"ExecutablePath", nullptr, &type, (LPBYTE)raw,
                             &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            // 统一路径形态：展开环境变量、去掉 \\??\\ 前缀与引号、统一大写
            std::wstring p = raw;
            if (p.compare(0, 4, L"\\??\\") == 0) p.erase(0, 4);
            if (!p.empty() && p.front() == L'"') p.erase(0, 1);
            if (!p.empty() && p.back()  == L'"') p.pop_back();
            wchar_t exp[MAX_PATH * 2] = L"";
            if (!ExpandEnvironmentStringsW(p.c_str(), exp, MAX_PATH * 2))
                lstrcpynW(exp, p.c_str(), MAX_PATH * 2);
            CharUpperBuffW(exp, (DWORD)lstrlenW(exp));
            bool same = !lstrcmpW(exp, exe);
            if (!same) {                    // 兼容 \\??\\ 设备路径：整路径后缀匹配
                size_t n = lstrlenW(exe), m = lstrlenW(exp);
                same = (m > n && lstrcmpW(exp + (m - n), exe) == 0);
            }
            if (!same) {                    // 兜底：文件名相同即视为本程序
                PCWSTR en = wcsrchr(exp, L'\\');
                en = en ? en + 1 : exp;
                same = !lstrcmpW(en, exeName);
            }
            if (same) {
                DWORD val = 0, vsz = sizeof(val), vtype = 0;
                if (RegQueryValueExW(k, L"IsPromoted", nullptr, &vtype,
                                     (LPBYTE)&val, &vsz) == ERROR_SUCCESS &&
                    vtype == REG_DWORD && val == 1) {
                    if (result == TRAY_PROMOTE_NONE) result = TRAY_PROMOTE_ALREADY;
                } else {
                    DWORD one = 1;
                    RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD,
                                   (const BYTE*)&one, sizeof(one));
                    result = TRAY_PROMOTE_WROTE;
                }
            }
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return result;
}

// 让「图标常驻右下角」真正生效（修复 v1.8.0 只写注册表不生效的问题）：
//  1) Explorer 在图标添加时读取 IsPromoted，事后写入不会立即反映 —— 所以
//     写入成功后立刻删除并重加图标，强制其按新设置渲染
//  2) Explorer 建立设置子键是异步的，首次运行时子键可能还没出现 —— 找不到
//     时用定时器（id 3）重试，最多 8 次 x 0.6 秒
//  3) v1.9.0：重加失败时立即进入快速重试（此前这条路径失败 = 图标永久
//     消失且无人知晓，是用户「后台运行没有托盘图标」的直接原因之一）；
//     重加成功后进入心跳验证期，验证期内再丢则回滚（见 RollbackTrayPromotion）
static void ApplyTrayPromotion(HWND hwnd) {
    if (g_trayRollback) return;          // 已回滚：本运行期走系统默认，不再提升
    for (int round = 0; round < 3; ++round) {
        int r = PromoteTrayIcon();
        if (r == TRAY_PROMOTE_NONE) {        // 子键还没就绪（或 Win10）
            if (hwnd && g_promoteTries++ < 8)
                SetTimer(hwnd, 3, 600, nullptr);
            return;
        }
        g_trayPromoted = true;
        if (r == TRAY_PROMOTE_ALREADY) return;   // 已是常驻状态，无需再动
        if (!hwnd) return;
        RemoveTrayIcon();                        // 重加一次，触发重新读取设置
        AddTrayIcon();
        if (!g_nidAdded) {                       // 重加失败：快速重试兜底
            EnsureTrayIcon(hwnd);
            return;
        }
        BeginTrayAliveCheck();                   // 心跳确认提升无害（10 秒）
    }
    PromoteTrayIcon();                           // 收尾：保证注册表最终为 1
}

// ---------------------------- 划词添加结果气泡（v1.12.0） --------------------
// 半透明置顶小窗：右下角弹出，2 秒后自动淡出消失；点击气泡打开主窗口
// v1.12.1：四角完全透明 —— 用 colorkey 抠掉圆角外区域（内容不含此色），
// 不再出现黑色四角；标题/正文字体加大；位置跟随主窗口所在显示器
static std::wstring g_toastTitle;     // 气泡标题（如「已添加到 Tickly」）
static std::wstring g_toastBody;      // 气泡正文（任务内容预览 / 失败原因）
static bool         g_toastOk = false;

static const COLORREF kToastKey = RGB(255, 0, 254);   // 透明色键（魔幻色）

static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == 1) {                        // 1 秒到：开始淡出
            KillTimer(h, 1);
            SetTimer(h, 2, 30, nullptr);
        } else if (w == 2) {                 // 约 8 帧淡出后销毁
            g_toastAlpha = (g_toastAlpha > 32) ? (BYTE)(g_toastAlpha - 32) : 0;
            if (g_toastAlpha <= 0) {
                KillTimer(h, 2);
                DestroyWindow(h);
            } else {
                SetLayeredWindowAttributes(h, kToastKey, g_toastAlpha,
                                           LWA_COLORKEY | LWA_ALPHA);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:                     // 点击气泡 -> 打开主窗口
        ShowMainWindow();
        DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;   // 背景由 WM_PAINT 统一填充（colorkey），避免四角黑块
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc {};
        GetClientRect(h, &rc);
        FillSolid(hdc, rc, kToastKey);       // 圆角外区域 = 透明色键 -> 完全透明
        // 圆角白底 + 细边框
        HGDIOBJ op = SelectObject(hdc, CachedPen(COL_BORDER));
        HGDIOBJ ob = SelectObject(hdc, CachedBrush(COL_WHITE));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, Scale(14), Scale(14));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        // 状态圆点：成功 = 绿底白勾；失败 = 红底白叹号
        int padX = Scale(14), padY = Scale(12);
        int d    = Scale(24);
        int cx   = padX + d / 2;
        int cy   = padY + Scale(20) / 2;
        RECT cr = { cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2 };
        FillSolid(hdc, cr, g_toastOk ? COL_ACCENT : COL_OVERDUE);
        HGDIOBJ oc = SelectObject(hdc, g_penCheck);
        if (g_toastOk) {
            MoveToEx(hdc, cr.left + MulDiv(d, 24, 100),
                           cr.top  + MulDiv(d, 53, 100), nullptr);
            POINT pts[2] = {
                { cr.left + MulDiv(d, 42, 100), cr.top + MulDiv(d, 71, 100) },
                { cr.left + MulDiv(d, 77, 100), cr.top + MulDiv(d, 31, 100) },
            };
            PolylineTo(hdc, pts, 2);
        } else {
            FillSolid(hdc, { cx - Scale(1), cy - Scale(6),
                             cx + Scale(1), cy + Scale(1) }, COL_WHITE);
            FillSolid(hdc, { cx - Scale(1), cy + Scale(3),
                             cx + Scale(1), cy + Scale(5) }, COL_WHITE);
        }
        SelectObject(hdc, oc);
        // 标题 + 正文（v1.12.1 字体加大：标题 15px 加粗 / 正文 14px；
        // 正文最多 2 行，超出省略号截断）
        int tx = padX + d + Scale(12);
        HGDIOBJ tf = SelectObject(hdc, g_fontToastT);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_TEXT);
        RECT tr = { tx, padY, rc.right - padX, padY + Scale(20) };
        DrawTextW(hdc, g_toastTitle.c_str(), -1, &tr,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, g_fontToastB);
        SetTextColor(hdc, COL_MUTED);
        RECT br = { tx, padY + Scale(20) + Scale(6),
                    rc.right - padX, rc.bottom - padY };
        DrawTextW(hdc, g_toastBody.c_str(), -1, &br,
                  DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(hdc, tf);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, 1);
        KillTimer(h, 2);
        if (g_hwndToast == h) g_hwndToast = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 弹出气泡（同一时刻至多一个：新气泡替换旧气泡）
static void ShowToast(const std::wstring& title, const std::wstring& body, bool ok) {
    if (!g_hwnd) return;
    if (g_hwndToast) { DestroyWindow(g_hwndToast); g_hwndToast = nullptr; }

    g_toastTitle = title;
    g_toastBody  = body;
    g_toastOk    = ok;

    int w    = Scale(340);
    int padX = Scale(14), padY = Scale(12);
    int d    = Scale(24);
    int textW = w - padX - d - Scale(12) - padX;
    // 量正文实际高度（限 2 行）
    int bodyH = Scale(19);
    HDC sdc = GetDC(nullptr);
    HGDIOBJ of = SelectObject(sdc, g_fontToastB);
    RECT calc = { 0, 0, textW, 0 };
    DrawTextW(sdc, body.c_str(), -1, &calc,
              DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_EDITCONTROL | DT_NOPREFIX);
    SelectObject(sdc, of);
    ReleaseDC(nullptr, sdc);
    bodyH = calc.bottom - calc.top;
    if (bodyH < Scale(19)) bodyH = Scale(19);
    if (bodyH > Scale(19) * 2) bodyH = Scale(19) * 2;
    int h = padY + Scale(20) + Scale(6) + bodyH + padY;

    // v1.13.0：显示在主窗口所在显示器工作区正中央（按需求改回居中）
    RECT wa {};
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - h) / 2;

    g_hwndToast = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"TicklyToast", L"", WS_POPUP,
        x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwndToast) return;
    g_toastAlpha = 242;
    // v1.12.1：colorkey + 整窗半透明组合 —— 圆角外四角完全透明
    SetLayeredWindowAttributes(g_hwndToast, kToastKey, g_toastAlpha,
                               LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(g_hwndToast, SW_SHOWNOACTIVATE);   // 不抢目标应用焦点
    SetTimer(g_hwndToast, 1, 1000, nullptr);      // v1.13.0：1 秒后自动淡出
}

// ---------------------------- 划词添加确认对话框（v0.15） --------------------
// 所选文本识别到日期时间时弹出：显示识别结果与任务内容，确认后按
// 识别到的日期时间入列（确认返回 true / 取消返回 false）。
// v0.20：识别结果可在弹窗内手动修改（日期/时间选择器），确认后按修改
// 后的值入列（通过 CapConfirmCtx 回传）。
// v0.21：任务内容同样可在弹窗内编辑（多行输入框，预填完整识别内容），
// 确认后按编辑后的内容入列（CapConfirmCtx::text 回传）。
// 对话框模板 DlgItem/BuildDialog 定义在「内存对话框模板」一节，
// ConfirmCapturedWhen 定义在其后，此处先前置声明供划词流程调用
struct CapConfirmCtx {
    long long due     = 0;    // 入列日期时间（识别值，可被弹窗修改）
    bool      hasTime = false;
    std::wstring text;        // v0.21：入列内容（弹窗内可编辑，确认时回传）
};
static const int IDC_DP3_DATE = 2207;  // v0.20 划词确认框：日期选择器
static const int IDC_DP3_TIME = 2208;  // v0.20 划词确认框：时间选择器
static bool ConfirmCapturedWhen(const std::wstring& whenStr,
                                const std::wstring& content,
                                CapConfirmCtx& ctx);
// v0.19：对话框正文按实际字体/宽度实测换行高度并放大窗口（定义在
// BuildDialog 之后，此处先前置声明供 CapConfirmDlgProc 调用）
static void GrowDialogToFitText(HWND h, int textId,
                                const std::vector<int>& btnIds,
                                int maxTextH = 0, bool forEdit = false);

static INT_PTR CALLBACK CapConfirmDlgProc(HWND h, UINT m, WPARAM w, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, GWLP_USERDATA, lp);
        // v0.20：识别到的日期时间预填到选择器，用户可直接修改（时间
        // 选择器带 DTS_SHOWNONE 复选框：未勾选 = 不带具体时间，与
        // 识别结果一致；勾选后可指定时:分）
        if (auto* ctx = (CapConfirmCtx*)lp) {
            tm d = LocalTmOf(ctx->due);
            SYSTEMTIME st {};
            st.wYear   = (WORD)(d.tm_year + 1900);
            st.wMonth  = (WORD)(d.tm_mon + 1);
            st.wDay    = (WORD)d.tm_mday;
            st.wHour   = (WORD)d.tm_hour;
            st.wMinute = (WORD)d.tm_min;
            SendMessageW(GetDlgItem(h, IDC_DP3_DATE), DTM_SETSYSTEMTIME,
                         GDT_VALID, (LPARAM)&st);
            HWND dTime = GetDlgItem(h, IDC_DP3_TIME);
            SendMessageW(dTime, DTM_SETSYSTEMTIME,
                         ctx->hasTime ? GDT_VALID : GDT_NONE, (LPARAM)&st);
            SendMessageW(dTime, DTM_SETFORMATW, 0, (LPARAM)L"H:mm");
        }
        // v0.19：任务内容可能折行超模板估算，先按实测换行高度放大窗口、
        // 按钮组随正文底部下移；v0.21 内容改为可编辑输入框 —— 高度上限
        // 约 7 行（窗口不无限加高），更长的内容在框内滚动查看
        int capH = 0;
        if (HWND ed = GetDlgItem(h, 102)) {
            HDC dc = GetDC(ed);
            if (dc) {
                HFONT f = (HFONT)SendMessageW(ed, WM_GETFONT, 0, 0);
                HGDIOBJ of = SelectObject(dc, f ? (HGDIOBJ)f
                                   : GetStockObject(DEFAULT_GUI_FONT));
                TEXTMETRICW tmc {};
                if (GetTextMetricsW(dc, &tmc))
                    capH = (tmc.tmHeight + tmc.tmExternalLeading) * 7 + 12;
                SelectObject(dc, of);
                ReleaseDC(ed, dc);
            }
        }
        GrowDialogToFitText(h, 102, { IDOK, IDCANCEL }, capH, true);
        // 热键触发时主窗口可能在后台/托盘：把确认框带到前台（同提醒弹窗）
        DWORD myT = GetCurrentThreadId();
        HWND  hFG = GetForegroundWindow();
        DWORD fgT = hFG ? GetWindowThreadProcessId(hFG, nullptr) : 0;
        if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(h);
        if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
        // v0.21：初始焦点给任务内容编辑框，可直接修改（Enter 确认 / Esc 取消）
        SetFocus(GetDlgItem(h, 102));
        return FALSE;                        // 已自行设置焦点
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            // v0.21：先读回（可能被编辑过的）任务内容 —— \r\n 归一为 \n、
            // 去首尾空白；内容被清空视为取消，不入列
            HWND ed = GetDlgItem(h, 102);
            int  tl = GetWindowTextLengthW(ed);
            std::wstring txt((size_t)(tl > 0 ? tl : 0) + 1, L'\0');
            if (tl > 0) GetWindowTextW(ed, &txt[0], tl + 1);
            txt.resize((size_t)(tl > 0 ? wcslen(txt.c_str()) : 0));
            std::wstring content;
            content.reserve(txt.size());
            for (wchar_t c : txt)
                if (c != L'\r') content += c;
            size_t b = content.find_first_not_of(L" \t\n");
            if (b == std::wstring::npos) {   // 清空 = 取消
                EndDialog(h, IDCANCEL);
                return TRUE;
            }
            size_t ce = content.find_last_not_of(L" \t\n");
            content = content.substr(b, ce - b + 1);
            // v0.20：确认时读取选择器 —— 日期恒有效（弹窗仅在识别到
            // 日期时出现）；时间未勾选则不带具体时间（入列当天 0 点）
            if (auto* ctx = (CapConfirmCtx*)GetWindowLongPtrW(h, GWLP_USERDATA)) {
                SYSTEMTIME sd {}, stt {};
                bool hasD = SendMessageW(GetDlgItem(h, IDC_DP3_DATE),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&sd) == GDT_VALID;
                bool hasT = SendMessageW(GetDlgItem(h, IDC_DP3_TIME),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&stt) == GDT_VALID;
                if (hasD)
                    ctx->due = MakeEpoch(sd.wYear, sd.wMonth, sd.wDay,
                                         hasT ? stt.wHour   : 0,
                                         hasT ? stt.wMinute : 0);
                ctx->hasTime = hasT;
                ctx->text    = content;      // v0.21：编辑后的内容回传
            }
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------- 划词添加任务（v1.12.0） ------------------------
// 全局快捷键触发：向前台应用模拟 Ctrl+C 捕获选中文本，加入任务列表并弹气泡。
// 说明：捕获会占用剪贴板（与用户手动 Ctrl+C 后的剪贴板状态一致）
static void CaptureSelectedToTask() {
    // 主窗口（或其子控件）在前台时不捕获，避免把界面文字捕获给自己
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || GetAncestor(fg, GA_ROOT) == g_hwnd) {
        MessageBeep(MB_ICONWARNING);
        ShowToast(L"未捕获到选中文本", L"请先在其他应用中选中文字，再按快捷键", false);
        return;
    }
    DWORD seq0 = GetClipboardSequenceNumber();
    // v0.28 记事本取词修复 —— 先试零注入的直接复制：
    // Alt 系热键触发时前台 Win32 程序已收到 Alt 按下，为让 Ctrl+C 干净
    // 而注入的 Alt 弹起会让经典记事本这类标准菜单程序进入菜单模态循环
    //（Alt 弹起 → SC_KEYMENU 是 Win32 菜单激活的标准路径），后续注入
    // 的 Ctrl+C 全被菜单循环吞掉，剪贴板永不变化。而 Win32 编辑控件
    //（类名含 Edit/RichEdit，记事本编辑区、写字板、Scintilla 系编辑器
    // 均如此）收到 WM_COPY 会直接把选中文本放入剪贴板 —— 这正是
    // Ctrl+C 在编辑控件内部的实现。因此对类名匹配的前台焦点控件直接发
    // WM_COPY（SendMessageTimeout 防目标挂起时阻塞自己），全程不注入
    // 按键；未命中再走下方通用组合键注入。
    bool changed = false;
    {
        GUITHREADINFO gi { sizeof(gi) };
        DWORD fgTid = GetWindowThreadProcessId(fg, nullptr);
        if (fgTid && GetGUIThreadInfo(fgTid, &gi) && gi.hwndFocus) {
            wchar_t cls[96] {};
            GetClassNameW(gi.hwndFocus, cls, 96);
            std::wstring lc = cls;
            CharLowerBuffW(lc.data(), (DWORD)lc.size());
            if (lc.find(L"edit") != std::wstring::npos ||
                lc.find(L"scintilla") != std::wstring::npos) {
                DWORD_PTR r = 0;
                SendMessageTimeoutW(gi.hwndFocus, WM_COPY, 0, 0,
                                    SMTO_ABORTIFHUNG, 300, &r);
                for (int i = 0; i < 16; ++i) {       // 最多约 240ms
                    if (i) Sleep(15);
                    if (GetClipboardSequenceNumber() != seq0) {
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
    // v0.18：不再等待修饰键物理松开（原最多 1.5 秒：热键在 Alt 按下瞬间
    // 触发，这段等待几乎每次都吃满到用户抬手，是弹窗延迟的大头）。
    // v1.12.1 已确认「注入修饰键弹起」才是确定性修复 —— 下面每次尝试的
    // 组合键都以同一批 SendInput 先注入 Alt/Shift/Win 弹起，到达目标
    // 应用的始终是干净的复制组合键；热键以 MOD_NOREPEAT 注册且在按下
    // 瞬间触发，Alt 的自动重复（≥500ms 后才开始）来不及插进注入序列
    // 向前台应用发送复制组合键（v1.13.0 Chrome 修复）：
    //  · 每个键之间留 25~45ms 间隔 —— Chromium 的输入管线对零间隔连发的
    //    Ctrl+C 偶尔丢键（记事本宽容，浏览器主进程 accelerator 需要完整
    //    的按下/抬起序列），逐键发送 + 间隔是各类划词工具的通用做法
    //  · 附带扫描码（wScan），个别按虚拟键过滤输入的应用也能收到
    //  · 最多 3 次尝试：Ctrl+C -> Ctrl+Insert -> Ctrl+C（第 2 次换备用
    //    复制快捷键，Chrome 对 Ctrl+Insert 同样响应）
    auto sendKey = [](WORD vk, bool up) {
        INPUT in {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (up) in.ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    };
    auto copyCombo = [&](bool insertInstead) {
        // v0.18：修饰键弹起与 Ctrl 按下合并为同一批 SendInput —— 弹起与
        // 组合键之间零间隙，物理上仍按住的 Alt/Shift 无法混进组合键；
        // Ctrl 按下与字符键按下之间仍保留 30ms 间隔（Chrome 修复需要）
        {
            INPUT b[4] = {};
            b[0].type = INPUT_KEYBOARD; b[0].ki.wVk = VK_MENU;
            b[0].ki.dwFlags = KEYEVENTF_KEYUP;
            b[1].type = INPUT_KEYBOARD; b[1].ki.wVk = VK_SHIFT;
            b[1].ki.dwFlags = KEYEVENTF_KEYUP;
            b[2].type = INPUT_KEYBOARD; b[2].ki.wVk = VK_LWIN;
            b[2].ki.dwFlags = KEYEVENTF_KEYUP;
            b[3].type = INPUT_KEYBOARD; b[3].ki.wVk = VK_CONTROL;
            SendInput(4, b, sizeof(INPUT));
        }
        Sleep(30);
        sendKey(insertInstead ? VK_INSERT : L'C', false); Sleep(45);
        sendKey(insertInstead ? VK_INSERT : L'C', true);  Sleep(30);
        sendKey(VK_CONTROL, true);  Sleep(25);
    };
    if (!changed) {                    // WM_COPY 未命中才注入复制组合键
        for (int attempt = 0; attempt < 3 && !changed; ++attempt) {
            copyCombo(attempt == 1);   // 第 2 次：Ctrl+Insert 备用快捷键
            // v0.18：组合键发出后立即检查（多数应用在 C 键按下后几十毫秒内
            // 就完成复制，往往早于组合键序列结束），未变化再每 15ms 轮询，
            // 每次尝试最多约 0.75 秒（原 40ms 一查，最快也要白等 40ms）
            for (int i = 0; i < 50; ++i) {
                if (i) Sleep(15);
                if (GetClipboardSequenceNumber() != seq0) {
                    changed = true;
                    break;
                }
            }
        }
    }
    if (!changed) {
        MessageBeep(MB_ICONWARNING);
        ShowToast(L"未捕获到选中文本", L"请先在其他应用中选中文字，再按快捷键", false);
        return;
    }
    // 读取剪贴板文本（目标应用可能短暂占用剪贴板，重试开板）
    std::wstring raw;
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(g_hwnd)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) {
                PCWSTR p = (PCWSTR)GlobalLock(hd);
                if (p) {
                    raw.assign(p);
                    GlobalUnlock(hd);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(50);
    }
    std::wstring text = NormalizeMultiline(raw);
    if (text.empty()) {
        MessageBeep(MB_ICONWARNING);
        ShowToast(L"未捕获到选中文本", L"所选内容不是文本（或内容为空）", false);
        return;
    }
    // v0.15：识别日期时间 —— 识别到时先弹窗确认，确认后按该日期时间入列。
    // v0.16：多行文本也识别（逐行扫描、取最优行），修复腾讯会议邀请函
    // 「会议时间：2026/10/14 14:30-15:30」识别不到的问题
    {
        WhenResult wr;
        std::wstring addText;
        bool recognized = false;
        if (text.find(L'\n') == std::wstring::npos) {
            wr = ParseWhen(text);
            if (wr.ok) {
                recognized = true;
                addText = !wr.stripped.empty() ? wr.stripped : text;
            }
        } else {
            std::wstring stripped;
            if (ParseWhenMultiline(text, wr, stripped)) {
                recognized = true;
                addText = !stripped.empty() ? stripped : text;
            }
        }
        if (recognized) {
            std::wstring whenStr = FormatWhen(wr.due, wr.hasTime, false, nullptr);
            // v0.21：完整任务内容传入确认框（多行保留换行），弹窗内可直接
            // 编辑；确认后按编辑过的内容入列，气泡同步展示编辑结果
            CapConfirmCtx ctx;
            ctx.due     = wr.due;
            ctx.hasTime = wr.hasTime;
            if (!ConfirmCapturedWhen(whenStr, addText, ctx))
                return;                     // 取消：不入列
            std::wstring finalText = NormalizeMultiline(ctx.text);
            if (finalText.empty()) return;  // 内容被清空 = 取消（弹窗已拦）
            Task t;
            t.text    = finalText;
            t.due     = ctx.due;
            t.hasTime = ctx.hasTime;
            g_tasks.insert(g_tasks.begin(), t);   // 新任务置顶
            g_multiSel.clear();                   // 下标整体 +1，多选失效
            SaveTasks();
            RebuildRows();
            UpdateButtons();
            CheckReminders();
            MessageBeep(MB_OK);
            // v0.20：气泡展示确认框内（可能被修改过的）日期时间
            std::wstring body = finalText;
            for (auto& c : body)              // 气泡单行展示：换行压平
                if (c == L'\n') c = L' ';
            if (body.size() > 80) body = body.substr(0, 80) + L"…";
            body += L"（" + FormatWhen(ctx.due, ctx.hasTime, false, nullptr) +
                    L"）";
            ShowToast(L"已添加到 Tickly", body, true);
            return;
        }
    }
    Task t;
    t.text = text;   // 所选即所得：无日期时间表达时不改动内容
    g_tasks.insert(g_tasks.begin(), t);   // 新任务置顶
    g_multiSel.clear();                   // 下标整体 +1，多选失效
    SaveTasks();
    RebuildRows();
    UpdateButtons();
    CheckReminders();
    std::wstring body = (text.size() > 80) ? text.substr(0, 80) + L"…" : text;
    MessageBeep(MB_OK);
    ShowToast(L"已添加到 Tickly", body, true);
}

// ---------------------------- 开机自启（注册表 HKCU Run） --------------------
static const wchar_t* kRunKey   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"Tickly";

static void SetAutoStart(bool on);   // 前置声明（GetAutoStart 里做旧值迁移）

static bool GetAutoStart() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k)
        != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD sz = sizeof(buf), type = 0;
    LRESULT r = RegQueryValueExW(k, kRunValue, nullptr, &type, (LPBYTE)buf, &sz);
    // v1.13.7：兼容旧版本（TodoWin）留下的自启项 —— 升级后开机自启不会
    // 静默失效；读到旧值就顺手迁成新值
    bool old = false;
    if (r != ERROR_SUCCESS) {
        sz = sizeof(buf); type = 0;
        r = RegQueryValueExW(k, L"TodoWin", nullptr, &type, (LPBYTE)buf, &sz);
        old = (r == ERROR_SUCCESS && type == REG_SZ);
    }
    RegCloseKey(k);
    if (old) SetAutoStart(true);   // 迁移（内部会清掉旧值）
    return r == ERROR_SUCCESS && type == REG_SZ;
}

static void SetAutoStart(bool on) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k)
        != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH * 2] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegDeleteValueW(k, L"TodoWin");   // v1.13.7：清掉改名前的旧自启项
    RegCloseKey(k);
}

// ---------------------------- 全局快捷键（v1.12.0） --------------------------
static const UINT HK_ID_SHOW = 11;   // 显示主窗口
static const UINT HK_ID_CAP  = 12;   // 划词添加任务

// 快捷键展示名：如「Alt+Q」「Ctrl+Shift+F1」
static std::wstring HotkeyName(UINT mod, UINT vk) {
    static const struct { UINT m; const wchar_t* n; } km[] = {
        { MOD_CONTROL, L"Ctrl+" }, { MOD_ALT, L"Alt+" },
        { MOD_SHIFT, L"Shift+" },  { MOD_WIN, L"Win+" },
    };
    std::wstring s;
    for (const auto& k : km)
        if (mod & k.m) s += k.n;
    wchar_t key[32] = L"";
    if ((vk >= L'0' && vk <= L'9') || (vk >= L'A' && vk <= L'Z')) {
        key[0] = (wchar_t)vk; key[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::swprintf(key, 32, L"F%d", (int)(vk - VK_F1 + 1));
    } else if (vk) {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (sc) GetKeyNameTextW((LONG)(sc << 16), key, 32);
    }
    if (!key[0]) return std::wstring();
    return s + key;
}

static bool RegisterOneHotkey(UINT id, UINT mod, UINT vk) {
    if (!g_hwnd || !vk || !mod) return false;
    return RegisterHotKey(g_hwnd, (int)id, mod | MOD_NOREPEAT, vk) != FALSE;
}

// 注册/重注册两个全局快捷键（启动、设置保存后调用）；结果记录在 g_hkShowOk/CapOk
static void ApplyHotkeys() {
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, HK_ID_SHOW);
    UnregisterHotKey(g_hwnd, HK_ID_CAP);
    g_hkShowOk = RegisterOneHotkey(HK_ID_SHOW, g_hkShowMod, g_hkShowVk);
    g_hkCapOk  = RegisterOneHotkey(HK_ID_CAP,  g_hkCapMod,  g_hkCapVk);
}

// 设置对话框里的快捷键生效状态行
static std::wstring HotkeyStatusText() {
    std::wstring a = HotkeyName(g_hkShowMod, g_hkShowVk);
    std::wstring b = HotkeyName(g_hkCapMod,  g_hkCapVk);
    if (a.empty()) a = L"未设置";
    if (b.empty()) b = L"未设置";
    return L"快捷键：显示 " + a + (g_hkShowOk ? L"（已生效）" : L"（被占用）")
         + L" · 划词 "  + b + (g_hkCapOk  ? L"（已生效）" : L"（被占用）");
}

// ---------------------------- 内存对话框模板 --------------------------------
struct DlgItem {
    DWORD          style;
    WORD           x, y, w, h;
    WORD           id;
    WORD           cls;      // 0x0080 button / 0x0081 edit / 0x0082 static
    const wchar_t* text;
    const wchar_t* clsName;  // 自定义窗口类名（cls==0 时使用，如日期选择器）
};
static const WORD CLS_BUTTON = 0x0080;
static const WORD CLS_EDIT   = 0x0081;
static const WORD CLS_STATIC = 0x0082;

struct DlgBuf {
    std::vector<WORD> w;
    void Word(WORD v)   { w.push_back(v); }
    void Dword(DWORD v) { Word(LOWORD(v)); Word(HIWORD(v)); }
    void Str(const wchar_t* s) { do { Word((WORD)*s); } while (*s++); }
    void Align4()       { while (w.size() & 1) Word(0); }
};

static DLGTEMPLATE* BuildDialog(const wchar_t* title, int cx, int cy,
                                const std::vector<DlgItem>& items,
                                DWORD exstyle = 0) {
    DlgBuf b;
    b.Dword((DWORD)(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
                    DS_SETFONT | DS_CENTER));
    b.Dword(exstyle);                    // 扩展样式（提醒弹窗 = WS_EX_TOPMOST）
    b.Word((WORD)items.size());
    b.Word(0); b.Word(0);                // x, y（DS_CENTER 忽略）
    b.Word((WORD)cx); b.Word((WORD)cy);
    b.Word(0);                           // 无菜单
    b.Word(0);                           // 窗口类
    b.Str(title);
    b.Word(9);                           // 9 pt
    b.Str(L"Microsoft YaHei UI");
    for (const DlgItem& it : items) {
        b.Align4();
        b.Dword(it.style);
        b.Dword(0);                      // 扩展样式
        b.Word(it.x); b.Word(it.y); b.Word(it.w); b.Word(it.h);
        b.Word(it.id);
        if (it.cls) {
            b.Word(0xFFFF); b.Word(it.cls);   // 预定义窗口类
        } else if (it.clsName && it.clsName[0]) {
            b.Str(it.clsName);                // 自定义类名（如日期选择器）
        } else {
            b.Word(0);
        }
        if (it.text && it.text[0]) b.Str(it.text); else b.Word(0);
        b.Word(0);                       // 无附加数据
    }
    size_t bytes = b.w.size() * sizeof(WORD);
    DLGTEMPLATE* tpl = (DLGTEMPLATE*)std::malloc(bytes);
    if (tpl) std::memcpy(tpl, b.w.data(), bytes);
    return tpl;                          // 调用者负责 free()
}

// v0.19：对话框正文完整可见 —— 模板高度按「每条 N 行」估算（对话单位
// 换算像素存在不确定性），大字体正文 / 长文本折行超估算行数时，底部
// 内容会被控件或窗口边缘截断。这里在 WM_INITDIALOG 里按正文控件当前
// 宽度与实际字体用 DrawText(DT_CALCRECT|DT_WORDBREAK) 实测换行总高：
// 需要更高时放大窗口（补上非客户区边框）、加高正文控件，按钮组整体
// 平移到新正文底部（保持原间距节奏）；估算足够时不做任何改动
typedef std::vector<int> DlgBtnIds;
// v0.21：maxTextH —— 正文控件高度上限（px，0 = 不限）；forEdit —— 正文是
// EDIT 控件（测宽扣除边框/内边距与垂直滚动条，余量加大防内滚）
static void GrowDialogToFitText(HWND h, int textId,
                                const DlgBtnIds& btnIds,
                                int maxTextH, bool forEdit) {
    HWND st = GetDlgItem(h, textId);
    if (!st || btnIds.empty()) return;
    HFONT f = (HFONT)SendMessageW(st, WM_GETFONT, 0, 0);
    RECT sr {}, cr {}, wr {}, cr0 {}, b0 {};
    HWND b0h = GetDlgItem(h, btnIds[0]);
    if (!b0h) return;
    GetWindowRect(st, &sr);
    GetClientRect(h, &cr);
    GetWindowRect(b0h, &b0);
    POINT tp = { sr.left, sr.top };
    ScreenToClient(h, &tp);
    POINT bp = { b0.left, b0.top };
    ScreenToClient(h, &bp);
    int left = tp.x, top = tp.y;
    int width = sr.right - sr.left;       // 保持模板宽度不变，只修高度
    if (forEdit)   // v0.21：EDIT 文字区比窗口窄（边框+内边距+垂直滚动条）
        width -= 8 + GetSystemMetrics(SM_CXVSCROLL);
    int oldH  = sr.bottom - sr.top;
    int gap   = bp.y - (top + oldH);      // 正文底 → 按钮顶（模板原间距）
    int btmM  = cr.bottom - (bp.y + (b0.bottom - b0.top));   // 按钮底 → 客户区底
    if (gap < 0) gap = 0;
    if (btmM < 0) btmM = 0;
    // 按实际字体在控件像素宽度内实测换行总高
    int len = GetWindowTextLengthW(st);
    std::wstring txt((size_t)(len > 0 ? len : 0) + 1, L'\0');
    if (len > 0) GetWindowTextW(st, &txt[0], len + 1);
    txt.resize((size_t)(len > 0 ? len : 0));
    HDC dc = GetDC(st);
    HGDIOBJ of = SelectObject(dc, f ? (HGDIOBJ)f
                                    : GetStockObject(DEFAULT_GUI_FONT));
    RECT calc = { 0, 0, width, 0 };
    DrawTextW(dc, txt.c_str(), -1, &calc,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, of);
    ReleaseDC(st, dc);
    int textH = (calc.bottom - calc.top) + (forEdit ? 8 : 2);   // 安全余量
    if (maxTextH > 0 && textH > maxTextH)
        textH = maxTextH;                 // v0.21：上限（超出部分框内滚动）
    if (textH <= oldH) return;            // 模板估算已够，维持原样
    int newBtnTop = top + textH + gap;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr0);
    int frameW = (int)(wr.right - wr.left) - (int)(cr0.right - cr0.left);
    int frameH = (int)(wr.bottom - wr.top) - (int)(cr0.bottom - cr0.top);
    SetWindowPos(h, nullptr, 0, 0,
                 (int)(cr0.right - cr0.left) + frameW,
                 newBtnTop + (int)(b0.bottom - b0.top) + btmM + frameH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(st, nullptr, 0, 0, width, textH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (int id : btnIds) {
        HWND bh = GetDlgItem(h, id);
        if (!bh) continue;
        RECT br {};
        GetWindowRect(bh, &br);
        POINT p = { br.left, br.top };
        ScreenToClient(h, &p);
        SetWindowPos(bh, nullptr, p.x, newBtnTop, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ---------------------------- 划词添加确认对话框（v0.15） --------------------
// 定义在 BuildDialog 之后（前置声明在划词添加流程处）：
// 确认返回 true（按确认时的日期时间入列），取消返回 false（不入列）。
// v0.20：弹窗内日期/时间选择器预填识别结果，可手动修改后确认
// v0.21：任务内容预填到多行编辑框，可直接修改后确认（content = 完整内容）
static bool ConfirmCapturedWhen(const std::wstring& whenStr,
                                const std::wstring& content,
                                CapConfirmCtx& ctx) {
    std::wstring editInit = ToCrLf(content);   // EDIT 只认 \r\n 换行
    std::vector<DlgItem> items = {
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 12, 208, 10, 100,
          CLS_STATIC, L"从所选文本识别到日期时间（可修改）：" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 24, 26, 196, 10, 101,
          CLS_STATIC, whenStr.c_str() },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 43, 26, 10, 100,
          CLS_STATIC, L"日期" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_SHORTDATECENTURYFORMAT,
          40, 40, 88, 13, (WORD)IDC_DP3_DATE, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 134, 43, 26, 10, 100,
          CLS_STATIC, L"时间" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_SHOWNONE|DTS_TIMEFORMAT,
          162, 40, 58, 13, (WORD)IDC_DP3_TIME, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 60, 208, 10, 100,
          CLS_STATIC, L"将添加的任务（可修改）：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_MULTILINE|
          ES_AUTOVSCROLL|WS_VSCROLL, 24, 74, 196, 10, 102,
          CLS_EDIT, editInit.c_str() },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
          48, 98, 62, 14, IDOK, CLS_BUTTON, L"确认添加" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          122, 98, 62, 14, IDCANCEL, CLS_BUTTON, L"取消" },
    };
    DLGTEMPLATE* tpl = BuildDialog(L"确认添加任务", 232, 122, items);
    // v0.17：确认框改为无属主弹出 —— 此前以主窗口为属主，确认框被激活时
    // 属主链一并激活/提升，托盘或后台的主界面会跟着弹到前台；快捷键划词
    // 场景下用户只想看到确认框本身，主界面应保持原状
    INT_PTR r = DialogBoxIndirectParam(GetModuleHandleW(nullptr), tpl,
                                       nullptr, CapConfirmDlgProc,
                                       (LPARAM)&ctx);
    std::free(tpl);
    return r == IDOK;
}

// ---------------------------- 设置对话框 ------------------------------------
static const int IDC_CK_REMIND  = 2101;  // 到点提醒
static const int IDC_ED_MIN     = 2102;  // 提前分钟数
static const int IDC_CK_AUTORUN = 2103;  // 开机自启
static const int IDC_CK_TRAYRUN = 2104;  // 关闭主窗口后托盘后台运行
static const int IDC_ED_TASKFONT = 2107; // v1.13.0 任务字体大小
static const int IDC_SW_TASK     = 2108; // v1.13.0 任务字体颜色色块（点击改色）
static const int IDC_ED_GRPFONT  = 2109; // v1.13.0 未完成分组字体大小
static const int IDC_SW_GROUP    = 2110; // v1.13.0 未完成分组字体颜色色块
static const int IDC_ED_GRPFONT_DONE = 2116; // v1.13.6 已完成分组字体大小
static const int IDC_SW_GROUP_DONE   = 2117; // v1.13.6 已完成分组字体颜色色块
static const int IDC_ED_DTFONT   = 2111; // v0.26 日期时间字体大小
static const int IDC_SW_DT       = 2112; // v0.26 日期时间字体颜色色块（点击改色）
static const int IDC_RB_THEME_L  = 2113; // v0.27 外观：浅色
static const int IDC_RB_THEME_D  = 2114; // v0.27 外观：深色
static const int IDC_RB_THEME_S  = 2115; // v0.27 外观：跟随系统（默认）

static INT_PTR CALLBACK SettingsDlgProc(HWND h, UINT m, WPARAM w, LPARAM lParam) {
    // v1.13.0：字体设置临时态（对话框生存期）+ 色块画刷 + 自定义色盘记忆
    static COLORREF sTaskCol = 0, sGrpCol = 0, sDtCol = 0, sDoneGrpCol = 0;
    static HBRUSH   sBrTask = nullptr, sBrGrp = nullptr, sBrDt = nullptr,
                    sBrGrpDone = nullptr;
    static COLORREF sCust[16] = {};
    switch (m) {
    case WM_INITDIALOG:
        sTaskCol = g_taskFontCol;
        sGrpCol  = g_grpFontCol;
        sDtCol   = g_dtFontCol;
        sDoneGrpCol = g_doneGrpFontCol;
        sBrTask  = CreateSolidBrush(sTaskCol);
        sBrGrp   = CreateSolidBrush(sGrpCol);
        sBrDt    = CreateSolidBrush(sDtCol);
        sBrGrpDone = CreateSolidBrush(sDoneGrpCol);
        CheckDlgButton(h, IDC_CK_REMIND, g_remindOn ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(h, IDC_ED_MIN, (UINT)(g_remindMin < 0 ? 0 : g_remindMin), FALSE);
        SetDlgItemInt(h, IDC_ED_TASKFONT, (UINT)g_taskFontSize, FALSE);
        SetDlgItemInt(h, IDC_ED_GRPFONT, (UINT)g_grpFontSize, FALSE);
        SetDlgItemInt(h, IDC_ED_GRPFONT_DONE, (UINT)g_doneGrpFontSize, FALSE);
        SetDlgItemInt(h, IDC_ED_DTFONT, (UINT)g_dtFontSize, FALSE);
        CheckDlgButton(h, IDC_CK_AUTORUN, GetAutoStart() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_CK_TRAYRUN, g_closeToTray ? BST_CHECKED : BST_UNCHECKED);
        // v0.27：外观（主题三态）单选钮初始态
        CheckRadioButton(h, IDC_RB_THEME_L, IDC_RB_THEME_S,
                         (g_themeMode == 0) ? IDC_RB_THEME_L :
                         (g_themeMode == 1) ? IDC_RB_THEME_D : IDC_RB_THEME_S);
        {
            // v1.12.0 采集框；v0.15 起固定 Alt 修饰：控件前显示不可修改的
            // 「Alt+」文本，采集框内按 Ctrl/Shift/Win 组合一律剥掉修饰键
            // 只保留主键（HKCOMB_* 全部禁用，替换为无修饰键）
            DWORD bad = HKCOMB_A | HKCOMB_C | HKCOMB_S |
                        HKCOMB_CA | HKCOMB_SA | HKCOMB_SC | HKCOMB_SCA;
            HWND hk1 = GetDlgItem(h, IDC_HK_SHOW);
            HWND hk2 = GetDlgItem(h, IDC_HK_CAPTURE);
            SendMessageW(hk1, HKM_SETRULES, bad, MAKELPARAM(0, 0));
            SendMessageW(hk2, HKM_SETRULES, bad, MAKELPARAM(0, 0));
            SendMessageW(hk1, HKM_SETHOTKEY, MAKEWPARAM((WORD)g_hkShowVk, 0), 0);
            SendMessageW(hk2, HKM_SETHOTKEY, MAKEWPARAM((WORD)g_hkCapVk, 0), 0);
        }
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        // v1.13.0：字体颜色色块用当前颜色画刷填充
        HWND ct = (HWND)lParam;
        int  id = (ct && IsWindow(ct)) ? GetDlgCtrlID(ct) : 0;
        if (id == IDC_SW_TASK && sBrTask) return (INT_PTR)sBrTask;
        if (id == IDC_SW_GROUP && sBrGrp) return (INT_PTR)sBrGrp;
        if (id == IDC_SW_GROUP_DONE && sBrGrpDone) return (INT_PTR)sBrGrpDone;
        if (id == IDC_SW_DT && sBrDt) return (INT_PTR)sBrDt;
        // v0.27：其余静态文本（含单选/复选标签）随主题配色；编辑框、
        // DTP、快捷键采集框等系统控件本期限随系统样式
        SetTextColor((HDC)w, COL_TEXT);
        SetBkColor((HDC)w, COL_BG);
        return (INT_PTR)CachedBrush(COL_BG);
    }
    case WM_CTLCOLORDLG:                 // v0.27：对话框背景随主题
        SetTextColor((HDC)w, COL_TEXT);
        SetBkColor((HDC)w, COL_BG);
        return (INT_PTR)CachedBrush(COL_BG);
    case WM_CTLCOLORBTN:                 // v0.27：按钮背景刷随主题（复选/单选周围底色）
        return (INT_PTR)CachedBrush(COL_BG);
    case WM_COMMAND:
        if (LOWORD(w) == IDC_SW_TASK || LOWORD(w) == IDC_SW_GROUP ||
            LOWORD(w) == IDC_SW_DT || LOWORD(w) == IDC_SW_GROUP_DONE) {
            if (HIWORD(w) != STN_CLICKED) break;
            // 0任务 / 1未完成组 / 2日期 / 3已完成组（v1.13.6）
            int which = (LOWORD(w) == IDC_SW_TASK) ? 0 :
                        (LOWORD(w) == IDC_SW_GROUP) ? 1 :
                        (LOWORD(w) == IDC_SW_DT) ? 2 : 3;
            CHOOSECOLORW cc { sizeof(cc) };
            cc.hwndOwner   = h;
            cc.lpCustColors = sCust;
            cc.rgbResult    = which == 0 ? sTaskCol : which == 1 ? sGrpCol :
                              which == 2 ? sDtCol   : sDoneGrpCol;
            cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc)) {
                COLORREF c = cc.rgbResult;
                if (which == 0) {
                    sTaskCol = c;
                    if (sBrTask) DeleteObject(sBrTask);
                    sBrTask = CreateSolidBrush(c);
                } else if (which == 1) {
                    sGrpCol = c;
                    if (sBrGrp) DeleteObject(sBrGrp);
                    sBrGrp = CreateSolidBrush(c);
                } else if (which == 2) {
                    sDtCol = c;
                    if (sBrDt) DeleteObject(sBrDt);
                    sBrDt = CreateSolidBrush(c);
                } else {
                    sDoneGrpCol = c;
                    if (sBrGrpDone) DeleteObject(sBrGrpDone);
                    sBrGrpDone = CreateSolidBrush(c);
                }
                InvalidateRect(GetDlgItem(h, LOWORD(w)), nullptr, TRUE);
            }
            return TRUE;
        }
        if (LOWORD(w) == IDC_RB_THEME_L || LOWORD(w) == IDC_RB_THEME_D ||
            LOWORD(w) == IDC_RB_THEME_S) {
            // v0.27：切外观立即生效并联存（F 行）——即使随后取消，
            // 外观选择也保留（与字体即时预览同权重）
            if (HIWORD(w) != BN_CLICKED) break;
            g_themeMode = (LOWORD(w) == IDC_RB_THEME_L) ? 0 :
                          (LOWORD(w) == IDC_RB_THEME_D) ? 1 : 2;
            ApplyTheme();                    // 界面立即生效
            SaveTasks();                     // 触发一次设置保存（F 行）
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            g_remindOn = IsDlgButtonChecked(h, IDC_CK_REMIND) == BST_CHECKED;
            UINT mv = GetDlgItemInt(h, IDC_ED_MIN, nullptr, FALSE);
            g_remindMin = (mv > 10080) ? 10080 : (int)mv;
            SetAutoStart(IsDlgButtonChecked(h, IDC_CK_AUTORUN) == BST_CHECKED);
            g_closeToTray = IsDlgButtonChecked(h, IDC_CK_TRAYRUN) == BST_CHECKED;
            // v1.13.0：字体大小（任务 12~22 / 分组 10~18 / 日期 10~18）与颜色
            UINT tf = GetDlgItemInt(h, IDC_ED_TASKFONT, nullptr, FALSE);
            UINT gf = GetDlgItemInt(h, IDC_ED_GRPFONT, nullptr, FALSE);
            UINT gfd = GetDlgItemInt(h, IDC_ED_GRPFONT_DONE, nullptr, FALSE);
            UINT df = GetDlgItemInt(h, IDC_ED_DTFONT, nullptr, FALSE);
            g_taskFontSize = (tf < 12) ? 12 : (tf > 22 ? 22 : (int)tf);
            g_grpFontSize  = (gf < 10) ? 10 : (gf > 18 ? 18 : (int)gf);
            g_doneGrpFontSize = (gfd < 10) ? 10 : (gfd > 18 ? 18 : (int)gfd);
            g_dtFontSize   = (df < 10) ? 10 : (df > 18 ? 18 : (int)df);
            g_taskFontCol  = sTaskCol;
            g_grpFontCol   = sGrpCol;
            g_doneGrpFontCol = sDoneGrpCol;
            g_dtFontCol    = sDtCol;
            ApplyFontSettings();             // 字体/行高/折行全部重算
            // v0.15：读取快捷键采集框并重注册 —— 修饰键固定 Alt，只取主键
            //（留空 = 保持不变）
            LRESULT k1 = SendMessageW(GetDlgItem(h, IDC_HK_SHOW), HKM_GETHOTKEY, 0, 0);
            LRESULT k2 = SendMessageW(GetDlgItem(h, IDC_HK_CAPTURE), HKM_GETHOTKEY, 0, 0);
            UINT vk1 = LOWORD(k1), vk2 = LOWORD(k2);
            if (vk1) { g_hkShowVk = vk1; g_hkShowMod = MOD_ALT; }
            if (vk2) { g_hkCapVk  = vk2; g_hkCapMod  = MOD_ALT; }
            ApplyHotkeys();
            SaveTasks();
            EndDialog(h, IDOK);
            CheckReminders();            // 设置可能变化，立即复查
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    case WM_NCDESTROY:
        if (sBrTask) { DeleteObject(sBrTask); sBrTask = nullptr; }
        if (sBrGrp)  { DeleteObject(sBrGrp);  sBrGrp  = nullptr; }
        if (sBrDt)   { DeleteObject(sBrDt);   sBrDt   = nullptr; }
        if (sBrGrpDone) { DeleteObject(sBrGrpDone); sBrGrpDone = nullptr; }
        break;
    }
    return FALSE;
}

static void OpenSettings() {
    // 状态自检行：同时让用户确认下载到了新版 + 托盘小图标当前状态
    //（版本号需与 resource.rc 的 ProductVersion 同步更新）
    wchar_t trayStat[128];
    if (!g_nidAdded)
        lstrcpynW(trayStat,
                  L"v1.13.24 · 托盘小图标添加失败，5 秒后自动重试；若一直失败请重启资源管理器", 128);
    else if (g_trayRollback)
        lstrcpynW(trayStat,
                  L"v1.13.24 · 图标已回退为系统默认：收纳在右下角「^」折叠区内，一定显示", 128);
    else if (g_trayPromoted)
        lstrcpynW(trayStat,
                  L"v1.13.24 · 托盘小图标常驻任务栏右下角（智能心跳保活）", 128);
    else
        lstrcpynW(trayStat,
                  L"v1.13.24 · 托盘小图标可能收纳在右下角「^」折叠区内（智能心跳保活）", 128);
    static std::wstring hkStat;               // 状态行持有的字符串（对话框生存期）
    hkStat = HotkeyStatusText();
    std::vector<DlgItem> items = {
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
          12, 12, 206, 10, (WORD)IDC_CK_REMIND, CLS_BUTTON, L"到时间弹出提醒框" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 31,  26, 10, 100, CLS_STATIC, L"提前" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
          42, 29,  34, 12, (WORD)IDC_ED_MIN, CLS_EDIT, L"15" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          82, 31, 136, 10, 100, CLS_STATIC, L"分钟后提醒（0 = 到点即提醒）" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
          12, 49, 206, 10, (WORD)IDC_CK_AUTORUN, CLS_BUTTON, L"开机自动启动" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
          12, 67, 206, 10, (WORD)IDC_CK_TRAYRUN, CLS_BUTTON,
          L"关闭主窗口后在托盘后台运行（提醒继续生效）" },
        // v1.12.0 两个全局快捷键；v0.15 起固定 Alt 修饰 —— 采集框前显示
        // 不可修改的「Alt+」文本，采集框只录主键
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12,  89, 74, 10, 100, CLS_STATIC, L"显示主窗口：" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          88,  89, 22, 10, 100, CLS_STATIC, L"Alt+" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP,
          112,  86, 70, 13, (WORD)IDC_HK_SHOW, 0, L"", L"msctls_hotkey32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 107, 74, 10, 100, CLS_STATIC, L"划词添加任务：" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          88, 107, 22, 10, 100, CLS_STATIC, L"Alt+" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP,
          112, 104, 70, 13, (WORD)IDC_HK_CAPTURE, 0, L"", L"msctls_hotkey32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 126, 206, 10, 100, CLS_STATIC, trayStat },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 140, 206, 10, 100, CLS_STATIC, hkStat.c_str() },
        // v1.13.0：字体大小与颜色（任务内容字体 / 分组标题字体 / v0.26 日期
        // 时间字体，点击色块弹系统取色器自定义颜色）
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 162, 54, 10, 100, CLS_STATIC, L"任务字体：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
          70, 160, 34, 12, (WORD)IDC_ED_TASKFONT, CLS_EDIT, L"14" },
        { WS_CHILD|WS_VISIBLE|SS_NOTIFY|WS_BORDER,
          112, 160, 34, 12, (WORD)IDC_SW_TASK, CLS_STATIC, L"" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          152, 162, 70, 10, 100, CLS_STATIC, L"点色块改颜色" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 181, 54, 10, 100, CLS_STATIC, L"未完成组：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
          70, 179, 34, 12, (WORD)IDC_ED_GRPFONT, CLS_EDIT, L"12" },
        { WS_CHILD|WS_VISIBLE|SS_NOTIFY|WS_BORDER,
          112, 179, 34, 12, (WORD)IDC_SW_GROUP, CLS_STATIC, L"" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          152, 181, 70, 10, 100, CLS_STATIC, L"点色块改颜色" },
        // v1.13.6：已完成分组（本周 / 上周 / 本月 / 上月 / 其他）单独一套
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 200, 54, 10, 100, CLS_STATIC, L"已完成组：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
          70, 198, 34, 12, (WORD)IDC_ED_GRPFONT_DONE, CLS_EDIT, L"12" },
        { WS_CHILD|WS_VISIBLE|SS_NOTIFY|WS_BORDER,
          112, 198, 34, 12, (WORD)IDC_SW_GROUP_DONE, CLS_STATIC, L"" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          152, 200, 70, 10, 100, CLS_STATIC, L"点色块改颜色" },
        // v0.26：日期时间字体（未完成任务行右侧的日期时间文字）
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 219, 54, 10, 100, CLS_STATIC, L"日期字体：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
          70, 217, 34, 12, (WORD)IDC_ED_DTFONT, CLS_EDIT, L"12" },
        { WS_CHILD|WS_VISIBLE|SS_NOTIFY|WS_BORDER,
          112, 217, 34, 12, (WORD)IDC_SW_DT, CLS_STATIC, L"" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          152, 219, 70, 10, 100, CLS_STATIC, L"点色块改颜色" },
        // v0.27：外观（主题三态）—— 浅色 / 深色 / 跟随系统，
        // 第一个单选钮带 WS_GROUP，切换立即生效并联存
        { WS_CHILD|WS_VISIBLE|SS_LEFT,
          12, 238, 34, 10, 100, CLS_STATIC, L"外观：" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON|WS_GROUP,
          48, 237, 44, 12, (WORD)IDC_RB_THEME_L, CLS_BUTTON, L"浅色" },
        { WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
          96, 237, 44, 12, (WORD)IDC_RB_THEME_D, CLS_BUTTON, L"深色" },
        { WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
          144, 237, 80, 12, (WORD)IDC_RB_THEME_S, CLS_BUTTON, L"跟随系统" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON|WS_GROUP,
          62, 253, 48, 14, IDOK, CLS_BUTTON, L"保存" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          118, 253, 48, 14, (WORD)IDCANCEL, CLS_BUTTON, L"取消" },
    };
    DLGTEMPLATE* tpl = BuildDialog(L"设置", 230, 278, items);
    DialogBoxIndirectParam(GetModuleHandleW(nullptr), tpl, g_hwnd,
                           SettingsDlgProc, 0);
    std::free(tpl);
}

// ---------------------------- 任务提醒 --------------------------------------
static std::wstring RelTime(long long due, long long now) {
    long long d = due - now;
    if (d <= 0) return L"已到时间";
    if (d < 60) return L"1 分钟内";
    long long mi = (d + 59) / 60;
    if (mi < 60) return L"还有 " + std::to_wstring(mi) + L" 分钟";
    long long hh = mi / 60, mm = mi % 60;
    std::wstring s = L"还有 " + std::to_wstring(hh) + L" 小时";
    if (mm) s += L" " + std::to_wstring(mm) + L" 分钟";
    return s;
}

static INT_PTR CALLBACK ReminderDlgProc(HWND h, UINT m, WPARAM w, LPARAM) {
    switch (m) {
    case WM_INITDIALOG: {
        // 大号加粗字体（正文 17px / 按钮 13px，均 FW_BOLD）
        SendMessageW(GetDlgItem(h, 100), WM_SETFONT, (WPARAM)g_fontRemindT, TRUE);
        SendMessageW(GetDlgItem(h, IDOK), WM_SETFONT, (WPARAM)g_fontRemindB, TRUE);
        SendMessageW(GetDlgItem(h, IDC_REMIND_DONE), WM_SETFONT,
                     (WPARAM)g_fontRemindB, TRUE);
        // v0.19：正文按实际字体与控件宽度重新测量并放大窗口 —— 模板高度按
        // 「每条 2 行」估算，长任务预览（22 字接近满宽）或多条提醒、文本含
        // 换行时会折成 3 行以上，底部内容被控件或窗口边缘截断；实测后放大
        // 窗口、按钮组随正文底部下移，长文本提醒框也完整显示全部内容
        GrowDialogToFitText(h, 100, { IDOK, IDC_REMIND_DONE });
        // v0.15：提醒框默认显示在屏幕正中央（主窗口所在显示器工作区居中）
        {
            HMONITOR mon = MonitorFromWindow(g_hwnd ? g_hwnd : h,
                                             MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi {};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                RECT r {};
                GetWindowRect(h, &r);
                int nx = mi.rcWork.left +
                         ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2;
                int ny = mi.rcWork.top +
                         ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2;
                SetWindowPos(h, nullptr, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        // 全局弹窗：置顶并强制抢占前台 —— 即使主窗口已缩到托盘/在后台，
        // 也能立即浮到所有窗口之上（Windows 对后台进程调用
        // SetForegroundWindow 会静默失败，AttachThreadInput 可绕过该限制）
        DWORD myT = GetCurrentThreadId();
        HWND  hFG = GetForegroundWindow();
        DWORD fgT = hFG ? GetWindowThreadProcessId(hFG, nullptr) : 0;
        if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(h);
        if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) { EndDialog(h, IDOK); return TRUE; }
        if (LOWORD(w) == IDC_REMIND_DONE) {   // v0.15：一键完成本次提醒的任务
            long long now = (long long)time(nullptr);
            for (const auto& it : g_remindItems) {
                for (size_t i = 0; i < g_tasks.size(); ++i) {
                    Task& t = g_tasks[i];
                    if (!t.done && t.due == it.first && t.text == it.second) {
                        t.done   = true;
                        t.doneAt = now;
                        break;
                    }
                }
            }
            g_remindItems.clear();
            SaveTasks();
            RebuildRows();
            UpdateButtons();
            EndDialog(h, IDC_REMIND_DONE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void CheckReminders() {
    if (!g_remindOn || g_remindOpen) return;
    long long now = (long long)time(nullptr);
    long long adv = (long long)g_remindMin * 60;
    std::vector<int> cand;
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (t.done || !t.due || t.notified) continue;
        if (now >= t.due - adv) cand.push_back((int)i);
    }
    if (cand.empty()) return;
    std::sort(cand.begin(), cand.end(), [](int a, int b) {
        return g_tasks[(size_t)a].due < g_tasks[(size_t)b].due;
    });
    std::wstring msg;
    g_remindItems.clear();
    for (int i : cand) {
        Task& t = g_tasks[(size_t)i];
        t.notified = true;               // 先标记：每个任务只提醒一次
        g_remindItems.emplace_back(t.due, t.text);   // v0.15「完成」按钮用
        bool od = false;
        std::wstring when = FormatWhen(t.due, t.hasTime, false, &od);
        std::wstring txt = (t.text.size() > 22) ? t.text.substr(0, 22) + L"…"
                                                : t.text;
        for (auto& c : txt)                  // v0.19：预览压平换行 —— 多行任务
            if (c == L'\n' || c == L'\r') c = L' ';   // 的预览保持单行结构
        if (!msg.empty()) msg += L"\r\n";
        msg += L"· " + txt + L"\r\n    " + when + L"（" + RelTime(t.due, now)
             + L"）";
    }
    SaveTasks();
    g_remindOpen = true;
    MessageBeep(MB_ICONEXCLAMATION);
    int textH = (int)cand.size() * 26 + 6;   // 每条 2 行（按大号加粗字体计高）
    std::vector<DlgItem> items = {
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 14, 12, 216, (WORD)textH, 100,
          CLS_STATIC, msg.c_str() },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
          54, (WORD)(12 + textH + 10), 64, 16, IDOK, CLS_BUTTON, L"知道了" },
        // v0.15：右侧「完成」按钮 —— 一键把本次提醒的任务标记为完成
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          126, (WORD)(12 + textH + 10), 64, 16, (WORD)IDC_REMIND_DONE,
          CLS_BUTTON, L"完成" },
    };
    // WS_EX_TOPMOST：全局置顶弹窗，应用在后台/托盘时也会盖过其他窗口弹出
    DLGTEMPLATE* tpl = BuildDialog(L"任务提醒", 244, 12 + textH + 10 + 16 + 12,
                                   items, WS_EX_TOPMOST);
    DialogBoxIndirectParam(GetModuleHandleW(nullptr), tpl, g_hwnd,
                           ReminderDlgProc, 0);
    std::free(tpl);
    g_remindOpen = false;
}

// ---------------------------- 修改日期时间对话框 ----------------------------
static const int IDC_DP_DATE    = 2201;  // 日期选择器
static const int IDC_DP_TIME    = 2202;  // 时间选择器
static const int IDC_BTN_CLRDUE = 2203;  // 清除日期
static const int IDC_DP2_DATE   = 2205;  // v0.15 完成日期编辑：日期选择器
static const int IDC_DP2_TIME   = 2206;  // v0.15 完成日期编辑：时间选择器

static INT_PTR CALLBACK DateDlgProc(HWND h, UINT m, WPARAM w, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, GWLP_USERDATA, lp);
        int idx = (int)lp;
        if (idx < 0 || idx >= (int)g_tasks.size()) return TRUE;
        const Task& t = g_tasks[(size_t)idx];
        HWND dDate = GetDlgItem(h, IDC_DP_DATE);
        HWND dTime = GetDlgItem(h, IDC_DP_TIME);
        if (!t.due) {
            // 无日期任务（点「＋ 日期」进入）：默认给出今天，时间留空，可直接调整保存
            tm n = LocalTmOf((long long)time(nullptr));
            SYSTEMTIME today {};
            today.wYear  = (WORD)(n.tm_year + 1900);
            today.wMonth = (WORD)(n.tm_mon + 1);
            today.wDay   = (WORD)n.tm_mday;
            SendMessageW(dDate, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&today);
            SendMessageW(dTime, DTM_SETSYSTEMTIME, GDT_NONE, 0);
        } else {
            tm d = LocalTmOf(t.due);
            SYSTEMTIME st {};
            st.wYear   = (WORD)(d.tm_year + 1900);
            st.wMonth  = (WORD)(d.tm_mon + 1);
            st.wDay    = (WORD)d.tm_mday;
            st.wHour   = (WORD)d.tm_hour;
            st.wMinute = (WORD)d.tm_min;
            SendMessageW(dDate, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
            SendMessageW(dTime, DTM_SETSYSTEMTIME,
                         t.hasTime ? GDT_VALID : GDT_NONE, (LPARAM)&st);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int idx = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (idx >= 0 && idx < (int)g_tasks.size()) {
                Task& t = g_tasks[(size_t)idx];
                SYSTEMTIME sd {}, stt {};
                bool hasD = SendMessageW(GetDlgItem(h, IDC_DP_DATE),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&sd) == GDT_VALID;
                bool hasT = SendMessageW(GetDlgItem(h, IDC_DP_TIME),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&stt) == GDT_VALID;
                if (hasD) {
                    int hh = hasT ? stt.wHour : 9;
                    int mm = hasT ? stt.wMinute : 0;
                    t.due      = MakeEpoch(sd.wYear, sd.wMonth, sd.wDay, hh, mm);
                    t.hasTime  = hasT;
                    t.notified = false;   // 时间变更后允许再次提醒
                } else {
                    t.due = 0;
                    t.hasTime = false;
                }
                SaveTasks();
                RebuildRows();
                UpdateButtons();
                CheckReminders();         // 新时间若已进入提醒窗口则提示
            }
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDC_BTN_CLRDUE) {
            SendMessageW(GetDlgItem(h, IDC_DP_DATE), DTM_SETSYSTEMTIME, GDT_NONE, 0);
            SendMessageW(GetDlgItem(h, IDC_DP_TIME), DTM_SETSYSTEMTIME, GDT_NONE, 0);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------- 修改完成日期对话框（v0.15） --------------------
// 已完成任务右键「修改完成日期」/ 单击其日期标签：只改 doneAt（完成时间），
// 不影响任务原本设定的截止日期
static INT_PTR CALLBACK DoneDateDlgProc(HWND h, UINT m, WPARAM w, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, GWLP_USERDATA, lp);
        int idx = (int)lp;
        if (idx < 0 || idx >= (int)g_tasks.size()) return TRUE;
        tm d = LocalTmOf(g_tasks[(size_t)idx].doneAt);
        SYSTEMTIME st {};
        st.wYear   = (WORD)(d.tm_year + 1900);
        st.wMonth  = (WORD)(d.tm_mon + 1);
        st.wDay    = (WORD)d.tm_mday;
        st.wHour   = (WORD)d.tm_hour;
        st.wMinute = (WORD)d.tm_min;
        SendMessageW(GetDlgItem(h, IDC_DP2_DATE), DTM_SETSYSTEMTIME,
                     GDT_VALID, (LPARAM)&st);
        SendMessageW(GetDlgItem(h, IDC_DP2_TIME), DTM_SETSYSTEMTIME,
                     GDT_VALID, (LPARAM)&st);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            int idx = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (idx >= 0 && idx < (int)g_tasks.size()) {
                Task& t = g_tasks[(size_t)idx];
                SYSTEMTIME sd {}, stt {};
                bool hasD = SendMessageW(GetDlgItem(h, IDC_DP2_DATE),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&sd) == GDT_VALID;
                bool hasT = SendMessageW(GetDlgItem(h, IDC_DP2_TIME),
                                         DTM_GETSYSTEMTIME, 0,
                                         (LPARAM)&stt) == GDT_VALID;
                if (hasD) {
                    // 时间未单独调整时保留原完成时刻的时分
                    tm old = LocalTmOf(t.doneAt);
                    int hh = hasT ? stt.wHour : old.tm_hour;
                    int mm = hasT ? stt.wMinute : old.tm_min;
                    t.doneAt = MakeEpoch(sd.wYear, sd.wMonth, sd.wDay, hh, mm);
                    SaveTasks();
                    RebuildRows();
                    UpdateButtons();
                }
            }
            EndDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void OpenDoneDateEditor(int idx) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return;
    std::vector<DlgItem> items = {
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 15, 28, 10, 100, CLS_STATIC, L"日期" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_SHORTDATECENTURYFORMAT,
          46, 12, 96, 13, (WORD)IDC_DP2_DATE, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 34, 28, 10, 100, CLS_STATIC, L"时间" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_TIMEFORMAT,
          46, 31, 96, 13, (WORD)IDC_DP2_TIME, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
          47, 56, 48, 14, IDOK, CLS_BUTTON, L"保存" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          119, 56, 48, 14, IDCANCEL, CLS_BUTTON, L"取消" },
    };
    DLGTEMPLATE* tpl = BuildDialog(L"修改完成日期", 214, 80, items);
    DialogBoxIndirectParam(GetModuleHandleW(nullptr), tpl, g_hwnd,
                           DoneDateDlgProc, (LPARAM)idx);
    std::free(tpl);
}

// 单击任务行的日期时间标签 -> 修改/清除日期时间
// v0.15：已完成任务显示的是完成日期 -> 转到「修改完成日期」对话框
static void OpenDateEditor(int idx) {
    if (idx < 0 || idx >= (int)g_tasks.size()) return;
    if (g_tasks[(size_t)idx].done) { OpenDoneDateEditor(idx); return; }
    std::vector<DlgItem> items = {
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 15, 28, 10, 100, CLS_STATIC, L"日期" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_SHOWNONE|DTS_SHORTDATECENTURYFORMAT,
          46, 12, 96, 13, (WORD)IDC_DP_DATE, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|SS_LEFT, 12, 34, 28, 10, 100, CLS_STATIC, L"时间" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|DTS_SHOWNONE|DTS_TIMEFORMAT,
          46, 31, 96, 13, (WORD)IDC_DP_TIME, 0, L"", L"SysDateTimePick32" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          12, 56, 52, 14, (WORD)IDC_BTN_CLRDUE, CLS_BUTTON, L"清除日期" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
          100, 56, 48, 14, IDOK, CLS_BUTTON, L"保存" },
        { WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
          154, 56, 48, 14, (WORD)IDCANCEL, CLS_BUTTON, L"取消" },
    };
    DLGTEMPLATE* tpl = BuildDialog(L"修改日期和时间", 214, 80, items);
    DialogBoxIndirectParam(GetModuleHandleW(nullptr), tpl, g_hwnd,
                           DateDlgProc, (LPARAM)idx);
    std::free(tpl);
}

// ---------------------------- 输入框光标垂直居中 ----------------------------
static void CenterInput() {
    if (!g_input) return;
    // v1.13.14：输入法组词期间绝不动格式矩形 —— EM_SETRECT 会让 EDIT 重排，
    // 把组合串的插入点甩回开头（输入「后天下午」时最明显）
    if (g_imeComposing) return;
    // 每次从完整客户区出发重算（EM_GETRECT 会返回上次修改过的矩形，不可作基准）
    RECT rc {};
    GetClientRect(g_input, &rc);
    HDC hdc = GetDC(g_input);
    HGDIOBJ of = SelectObject(hdc, g_font);
    TEXTMETRICW tm {};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, of);
    ReleaseDC(g_input, hdc);
    int boxH = rc.bottom - rc.top;
    // v1.12.0：多行内容从顶部排布（仅留小边距）；单行保持垂直居中
    int lines = (int)SendMessageW(g_input, EM_GETLINECOUNT, 0, 0);
    int off = (lines > 1) ? Scale(5) : (boxH - tm.tmHeight) / 2;
    if (off < 0) off = 0;
    RECT rc2 = rc;
    rc2.top += off;
    rc2.bottom -= off;
    rc2.left  += Scale(8);   // v0.27：去 CLIENTEDGE 后文字与描边保持间距
    rc2.right -= Scale(8);   //（与 EM_SETMARGINS 同值，占位符 EM_GETRECT 同源）
    if (rc2.bottom < rc2.top + tm.tmHeight) rc2.bottom = rc2.top + tm.tmHeight;

    // v1.13.14：格式矩形没变就不要重设 —— 每次键入都会走 EN_CHANGE ->
    // UpdateInputSize -> CenterInput，而 EM_SETRECT 会让 EDIT 重算排版，
    // 输入「后天下午」这类会被实时识别的词时，光标会被甩回文本开头
    static RECT s_lastRect {};
    static bool s_lastValid = false;
    if (s_lastValid && EqualRect(&rc2, &s_lastRect)) return;
    s_lastRect  = rc2;
    s_lastValid = true;

    // v1.13.17：删掉「先存选区、SETRECT 后写回」这段 —— 写回的是读取瞬间的值，
    // 如果那一刻正处在输入法提交过程中，读到的是组合串起点，写回去等于把插入
    // 点顶到最前面，正是用户看到的「文字插到『后』前面」。EDIT 内部按字符索引
    // 维护插入点，EM_SETRECT 本身不会让它归零，恢复逻辑只有害处
    SendMessageW(g_input, EM_SETRECT, 0, (LPARAM)&rc2);
}

// ---------------------------- 子类化过程 ------------------------------------
// v0.27：输入框占位符文案 —— 空且未聚焦时在框内绘制（次级色）
static const wchar_t* kInputPlaceholder = L"添加新任务，回车确认…";

// 占位符绘制：先走默认过程完成自身绘制，再叠画在格式矩形内。
// 位置与真实文字同源（EM_GETRECT 返回 CenterInput 设置的格式矩形）
static void DrawInputPlaceholder(HWND h) {
    if (GetWindowTextLengthW(h) != 0 || GetFocus() == h) return;
    RECT fr;
    SendMessageW(h, EM_GETRECT, 0, (LPARAM)&fr);
    HDC dc = GetDC(h);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HGDIOBJ of = SelectObject(dc, f ? (HGDIOBJ)f
                                    : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_MUTED);
    DrawTextW(dc, kInputPlaceholder, -1, &fr,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
}

// 普通字符输入 / 粘贴的插入点兜底。
// 重要：不要把 WM_IME_CHAR / WM_IME_COMPOSITION 交给这个函数。
// IME 可能先发送多条中间组合串消息，再发送最终结果；此时每条消息后的文本
// 长度差并不等于这次组词最终插入的字符数。若在 IME 消息中调用 EM_SETSEL，
// 就可能按旧的组词起点反复设置插入点，使光标跳回文本前面。
// 输入法消息必须由原生 EDIT/IME 自己管理。输入期间的尺寸与格式矩形更新已由
// g_imeComposing、UpdateInputSize、CenterInput 及异步 EN_CHANGE 路径隔离。
static LRESULT PassKeepCaret(HWND h, UINT m, WPARAM w, LPARAM l) {
    DWORD b0 = 0, b1 = 0;
    CallWindowProcW(g_oldEditProc, h, EM_GETSEL, (WPARAM)&b0, (LPARAM)&b1);
    DWORD base = b0;
    int lenBefore = (int)CallWindowProcW(g_oldEditProc, h, WM_GETTEXTLENGTH, 0, 0);

    LRESULT r = CallWindowProcW(g_oldEditProc, h, m, w, l);

    int lenAfter = (int)CallWindowProcW(g_oldEditProc, h, WM_GETTEXTLENGTH, 0, 0);
    int added = lenAfter - lenBefore;
    if (added > 0) {
        DWORD want = base + (DWORD)added;
        if ((int)want > lenAfter) want = (DWORD)lenAfter;
        DWORD a0 = 0, a1 = 0;
        CallWindowProcW(g_oldEditProc, h, EM_GETSEL, (WPARAM)&a0, (LPARAM)&a1);
        if (a1 != want)
            CallWindowProcW(g_oldEditProc, h, EM_SETSEL, want, want);
    }
    return r;
}

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // v1.13.22：拦截外部发来的 EM_SETSEL(0,0)。
    //
    // 由插入点追踪日志实测确认：输入「后天下午」时四个字全部正确插入，插入点
    // 也已正确停在末尾 (4,4)；紧接着会收到一条 0x00B1（EM_SETSEL）把选区改成
    // (0,0)，而文本长度仍是 4。这条消息不来自本程序任何调用点（PowerCaret 与
    // Layout 的每次 EM_SETSEL 都在追踪范围内且值正确），是输入法在上屏收尾时
    // 发来的「插入点纠正」。它一旦生效，光标就停在开头，下一段组词从 0 位置
    // 开始提交 —— 这正是「光标回到最前面 / 文字插到『后』前面」的成因。
    //
    // 判定条件刻意收得很窄，避免误伤正常操作：
    //   · 只拦 w==0 && l==0（把光标放到开头并取消选择）
    //   · 只在文本非空、且当前插入点正好在文本末尾时拦
    // 用户主动点击 / Home 键 / 拖选都不走这条路径（那些是鼠标或键盘消息，由
    // EDIT 内部直接更新选区）；Layout 的选区恢复若真是 (0,0)，说明插入点本就
    // 在开头，此时 b1 不会是末尾，也不会命中。
    if (m == EM_SETSEL && (int)w == 0 && (int)l == 0) {
        DWORD b0 = 0, b1 = 0;
        CallWindowProcW(g_oldEditProc, h, EM_GETSEL, (WPARAM)&b0, (LPARAM)&b1);
        if (b1 > 0) {
            int lenNow = (int)CallWindowProcW(g_oldEditProc, h,
                                             WM_GETTEXTLENGTH, 0, 0);
            if (b1 == (DWORD)lenNow) return 0;    // 拒绝复位，保住末尾插入点
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == L'\n')) {
        // v0.16：Shift+Enter = 在输入框内换行（多行 EDIT 默认过程插入换行），
        // Enter = 添加任务
        if (GetKeyState(VK_SHIFT) & 0x8000)
            return CallWindowProcW(g_oldEditProc, h, m, w, l);
        AddFromInput();
        return 0;
    }
    if (m == WM_TIMER && w == 3) {   // v1.13.1：蓝色光标闪烁
        CaretBlink(h);
        return 0;
    }
    // 输入法组词期间冻结内边距/尺寸重算，但不自行保存/恢复 IME 插入点。
    // 原生 EDIT 与输入法负责组合串及最终结果的插入点；子类不应在这些消息中
    // 用 EM_SETSEL 覆盖它。
    if (m == WM_IME_STARTCOMPOSITION) {
        g_imeComposing = true;
    } else if (m == WM_IME_ENDCOMPOSITION) {
        g_imeComposing = false;
        // 延迟补算（绝不在 END 消息里同步重排）。即使 EN_CHANGE 投递的消息
        // 已在组词标志清除前执行并提前返回，这条消息也会在组词结束后再次补算。
        if (g_hwnd && IsWindow(g_hwnd))
            PostMessageW(g_hwnd, WM_INPUTCHANGED, 0, 0);
    }
    if (m == WM_PAINT) {         // v0.27：空且未聚焦时叠画占位符
        LRESULT r = CallWindowProcW(g_oldEditProc, h, m, w, l);
        DrawInputPlaceholder(h);
        HideSystemCaret(h);      // v1.13.1：压掉系统黑光标，改画蓝线
        PaintBlueCaret(h);
        return r;
    }
    if (m == WM_SETFOCUS) {
        g_imeComposing = false;  // 输入法被强制取消时不会发 ENDCOMPOSITION
        DeselectList();          // 点到输入框等其他位置 -> 取消任务选中
        LRESULT r = CallWindowProcW(g_oldEditProc, h, m, w, l);
        InvalidateRect(h, nullptr, TRUE);             // 占位符隐藏
        InvalidateRect(g_hwnd, &g_inputFrame, FALSE); // 描边变强调色
        AttachBlueCaret(h);                           // v1.13.1：接管光标
        return r;
    }
    if (m == WM_KILLFOCUS) {     // v0.27：焦点离开 -> 占位符/描边重绘
        DetachBlueCaret();                             // v1.13.1：交还光标
        LRESULT r = CallWindowProcW(g_oldEditProc, h, m, w, l);
        InvalidateRect(h, nullptr, TRUE);             // 占位符显示
        InvalidateRect(g_hwnd, &g_inputFrame, FALSE); // 描边回边框色
        return r;
    }
    // 普通字符和粘贴可使用插入点兜底；IME 消息必须由原生 EDIT/输入法处理，
    // 不在组词串更新或提交过程中主动发送 EM_SETSEL。
    LRESULT r;
    if (m == WM_CHAR || m == WM_PASTE)
        r = PassKeepCaret(h, m, w, l);
    else
        r = CallWindowProcW(g_oldEditProc, h, m, w, l);
    // v1.13.1：插入点/内容变了 —— 蓝线回到可见相位并重绘
    if (m == WM_KEYDOWN || m == WM_CHAR || m == WM_PASTE || m == WM_UNDO ||
        m == WM_CLEAR || m == WM_SETTEXT || m == WM_LBUTTONDOWN ||
        m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK)
        CaretKick(h);
    if (m == WM_MOUSEMOVE && (GetKeyState(VK_LBUTTON) & 0x8000))
        CaretKick(h);
    return r;
}

// ---------------------------- 自绘列表窗口（v1.13.0） ------------------------
// 前置声明（绘制函数定义在本过程之后）
static void DrawHeaderRow(HDC hdc, const RECT& rc, int grp);
static void DrawDoneSummaryRow(HDC hdc, const RECT& rc);   // v1.13.11
static void DrawTaskRow(HDC hdc, const RECT& rc, const Task& t, bool sel,
                        bool rowHover, bool hoverBox, bool divider, int idx);

// 悬浮滚动条：右缘 16px 为热区，thumb 宽 7px 圆角胶囊，距右缘 4px
static RECT SbThumbRect() {
    RECT crc;
    if (!g_list || !GetClientRect(g_list, &crc) || crc.bottom <= 0)
        return RECT{ 0, 0, 0, 0 };
    double maxPx = MaxScrollPx();
    int trackLen = crc.bottom - Scale(8);
    if (maxPx <= 0 || trackLen <= Scale(30)) return RECT{ 0, 0, 0, 0 };
    int thumbH = (int)((double)g_viewH * trackLen / g_totalH);
    if (thumbH < Scale(28)) thumbH = Scale(28);
    if (thumbH > trackLen) thumbH = trackLen;
    int maxThumb = trackLen - thumbH;
    int ty = Scale(4) + (int)(g_scrollPx * maxThumb / maxPx);
    int tx = crc.right - Scale(11);
    return RECT{ tx, ty, tx + Scale(7), ty + thumbH };
}

static bool SbHit(int x) {
    RECT crc;
    if (!g_list || !GetClientRect(g_list, &crc)) return false;
    return MaxScrollPx() > 0 && x >= crc.right - Scale(16);
}

// 内容坐标 -> 显示行（键盘翻页用）
static int RowFromContentY(int cy) {
    int n = (int)g_rows.size();
    if (n == 0 || g_rowTops.empty()) return -1;
    if (cy < 0) cy = 0;
    if (cy >= g_totalH) cy = g_totalH - 1;
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_rowTops[mid] <= cy) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static LRESULT CALLBACK TodoListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_GETDLGCODE:                      // 方向键交给自己（键盘滚动/移动选中）
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_TIMER:
        if (w == 2 && g_sbTimer) {           // v0.24：自动隐藏倒计时到点
            KillTimer(h, 2);
            g_sbTimer = 0;
            if (g_sbVis && (g_sbDrag || g_sbTrack)) {
                g_sbTimer = SetTimer(h, 2, 900, nullptr);  // 交互中：再等一拍
            } else if (g_sbVis) {
                g_sbVis = false;             // 交互结束：自动隐藏
                ListRepaint();
            }
            return 0;
        }
        if (w == 1 && g_scrollTimer) {       // 滚轮缓动动画帧（~60fps）
            g_scrollPx += (g_scrollTgt - g_scrollPx) * 0.45;
            double diff = g_scrollTgt - g_scrollPx;
            if (diff < 0) diff = -diff;
            if (diff < 0.6) {
                g_scrollPx = g_scrollTgt;
                KillTimer(h, 1);
                g_scrollTimer = 0;
            }
            ListRepaint();
            RepositionRowEditor();               // v0.17：滚动帧让行内编辑框跟随所在行
            KeepRowEditorInView();               // v1.13.3：整行滚出视口才关闭
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;                            // 背景由 WM_PAINT 双缓冲统一绘制
    case WM_CTLCOLOREDIT: {                  // v0.24：编辑框底色与选中行同色
        HDC hdc = (HDC)w;                    //（避免白框叠蓝底出现两种颜色）
        if (g_rowEdit && (HWND)l == g_rowEdit) {
            bool done = g_rowEditIdx >= 0 &&
                        g_rowEditIdx < (int)g_tasks.size() &&
                        g_tasks[(size_t)g_rowEditIdx].done;
            SetBkColor(hdc, COL_SEL_BG);
            SetTextColor(hdc, done ? COL_DONE : EffTaskCol());   // v0.27 同源
            return (LRESULT)CachedBrush(COL_SEL_BG);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT crc;
        GetClientRect(h, &crc);
        int W = crc.right - crc.left, H = crc.bottom - crc.top;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W > 0 ? W : 1, H > 0 ? H : 1);
        HGDIOBJ ob = SelectObject(mem, bmp);
        FillSolid(mem, crc, COL_WHITE);
        if (!g_rows.empty()) {
            int r0 = RowFromY(crc.top), r1 = RowFromY(crc.bottom - 1);
            if (r0 < 0) r0 = 0;
            for (int r = r0; r <= r1 && r < (int)g_rows.size(); ++r) {
                RECT ir = RowRect(r);
                int v = g_rows[r];
                if (v == ROW_FILL) {
                    FillSolid(mem, ir, COL_WHITE);
                } else if (v == ROW_DONE_SUMMARY) {
                    DrawDoneSummaryRow(mem, ir);
                } else if (v < 0) {
                    DrawHeaderRow(mem, ir, -v - 1);
                } else {
                    // 任务间分隔线：仅当下一行也是任务行时绘制
                    size_t nx = (size_t)r + 1;
                    bool div = nx < g_rows.size() && g_rows[nx] >= 0;
                    // v0.27：整行悬停（未选中时画悬停底色）
                    bool rowHover = (r == g_hoverLine);
                    DrawTaskRow(mem, ir, g_tasks[(size_t)v],
                                (r == g_selRow || MultiHas(v)),   // v0.16：多选高亮
                                rowHover,
                                (r == g_hoverRow && g_hoverBox), div, v);
                }
            }
        }
        // v1.13.11：空列表提示（有任务时不显示）
        if (g_tasks.empty()) {
            SetBkMode(mem, TRANSPARENT);
            HGDIOBJ oe = SelectObject(mem, g_fontSub);
            SetTextColor(mem, COL_MUTED);
            RECT er1 = { crc.left, crc.top + Scale(64), crc.right,
                         crc.top + Scale(64) + Scale(20) };
            DrawTextW(mem, L"还没有任务", -1, &er1,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(mem, COL_GRP_CNT);
            RECT er2 = { crc.left, crc.top + Scale(88), crc.right,
                         crc.top + Scale(88) + Scale(20) };
            DrawTextW(mem, L"在上方输入框写下第一件事，回车即可添加", -1, &er2,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(mem, oe);
        }
        if (MaxScrollPx() > 0 && g_sbVis) {  // v0.24：悬浮滚动条 thumb（自动隐藏）
            RECT tb = SbThumbRect();
            if (tb.bottom > tb.top) {
                COLORREF sc = (g_sbDrag || g_sbTrack) ? COL_SB_HOT : COL_SB;
                HGDIOBJ ob2 = SelectObject(mem, CachedBrush(sc));
                HGDIOBJ op2 = SelectObject(mem, CachedPen(sc));
                RoundRect(mem, tb.left, tb.top, tb.right, tb.bottom,
                          Scale(7), Scale(7));
                SelectObject(mem, op2);
                SelectObject(mem, ob2);
            }
        }
        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:                            // 视口变化：填塞行/前缀和/滚动钳制
        RebuildRowTops();
        if (g_rowEdit) {                     // v0.23：视口/宽度变了，封顶容量与
            SyncRowEditorSize();             // 折行都随视口重算，编辑框重摆跟随
            RepositionRowEditor();
        }
        ListRepaint();
        return 0;
    case WM_ROWEDITCHK:                      // v0.22：编辑框首帧后复核内容行数
        if (g_rowEdit) SyncRowEditorSize();  //（兜底 EDIT 懒折行的残存时序）
        return 0;
    case WM_MOUSEWHEEL: {                    // 平滑滚动：目标像素 + 缓动动画
        SbPoke();                 // v0.24：滚轮唤醒 thumb（边界处也给反馈）
        // v1.13.3：不再「一滚就退出编辑」—— 编辑框跟着行一起滚（缓动帧里
        // 由 RepositionRowEditor 重摆），只有整行滚出视口才保存关闭
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        ListScrollTo(g_scrollTgt + (double)(-delta) * Scale(96) / WHEEL_DELTA,
                     true);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        CloseRowEditor(true);                // 正在行内编辑则先保存关闭
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        // 悬浮滚动条：点击热区 = 抓取拖动（不算空白位置，不触发自动收起；
        // 不抢焦点，同苹果风格 —— 点滚动条不打断当前输入）
        if (SbHit(x)) {
            CloseRowEditor(true);
            RECT tb = SbThumbRect();
            g_sbDrag = true;
            SbPoke();                        // v0.24：盲抓热区立即显示 thumb
            SetCapture(h);
            g_sbGrabOff = (tb.bottom > tb.top) ? (y - tb.top) : Scale(3);
            ListRepaint();
            return 0;
        }
        SetFocus(h);                         // 点击行区域才接管键盘焦点
        int row = RowFromY(y);
        if (row < 0 || row >= (int)g_rows.size()) { DeselectList(); return 0; }
        int v = g_rows[row];
        if (v == ROW_FILL) {                 // 填塞区 = 空白：取消选中+自动收起
            if (CollapseExpandedExcept(-1)) RefreshListKeepSel();
            DeselectList();
            return 0;
        }
        if (v == ROW_DONE_SUMMARY) {         // v1.13.11：已完成总标题，点击展开/收起
            g_doneCollapsed = !g_doneCollapsed;
            SaveTasks();
            RebuildRows();
            DeselectList();
            return 0;
        }
        if (v < 0) {                         // 分组标题：导出热区 / 折叠展开
            int grp = -1 - v;
            RECT ir = RowRect(row);
            if (HeaderExportHit(grp, ir, x)) { ExportGroup(grp); return 0; }
            ToggleGroup(grp);
            DeselectList();
            return 0;
        }
        int idx = v;
        RECT ir = RowRect(row);
        bool expandBtn = ExpandBtnHit(idx, ir, x, y);
        // v1.13.1：收起（或本行展开/收起）会让上方行高骤减，被点的这一行
        // 整体向上跳 —— 先记住它的视口位置，重建后原样还原，双击落点才不会漂移
        int anchorY = RowAnchorY(idx);
        bool wasExpanded = g_tasks[(size_t)idx].expanded;
        // v1.13.0：点击任务行（非本任务的展开按钮场景）时收起其他已展开任务
        bool chg = CollapseExpandedExcept(idx);
        if (expandBtn) {
            Task& t = g_tasks[(size_t)idx];
            t.expanded = !t.expanded;
            chg = true;
        }
        if (chg) {
            RefreshListKeepSel();
            row = RowOfIdx(idx);
            if (row < 0) { UpdateButtons(); return 0; }
            ir = RowRect(row);
            if (expandBtn && !wasExpanded) {
                // v1.13.4：展开 -> 行顶滚到视口顶部，从第一行看起
                ListScrollTo((double)RowTop(row), true);
            } else {
                RestoreRowAnchor(idx, anchorY);   // 收起 / 收起他行：行顶原位
            }
        }
        if (!expandBtn) {
            if (PtInBoxZone(ir, x)) {        // 单击勾选框 -> 切换完成状态
                g_multiSel.clear();          // v0.16：勾选属单任务操作，清多选
                g_anchorRow = row;
                g_selRow = row;
                ToggleTask(row);
                return 0;
            }
            if (DateLabelHit(idx, ir, x)) {  // 单击日期标签 -> 修改日期时间
                g_multiSel.clear();          // v0.16：改日期属单任务操作
                g_anchorRow = row;
                g_selRow = row;
                UpdateButtons();
                ListRepaint();
                PostMessageW(g_hwnd, WM_OPENDATE, (WPARAM)idx, 0);
                return 0;
            }
        }
        // v0.16：普通单击 / Ctrl+单击 / Shift+单击 三种选中方式
        {
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            if (ctrl) {                      // Ctrl+单击：逐个多选（切换）
                if (g_multiSel.empty() && g_selRow >= 0 &&
                    g_selRow < (int)g_rows.size() && g_rows[g_selRow] >= 0 &&
                    g_rows[g_selRow] != idx)
                    g_multiSel.push_back(g_rows[g_selRow]);   // 原选中为第一项
                auto it = std::find(g_multiSel.begin(), g_multiSel.end(), idx);
                if (it != g_multiSel.end()) g_multiSel.erase(it);
                else g_multiSel.push_back(idx);
                std::sort(g_multiSel.begin(), g_multiSel.end());
                g_selRow    = row;
                g_anchorRow = row;
            } else if (shift) {              // Shift+单击：锚点 -> 本行批量选
                MultiSelectRange((g_anchorRow >= 0) ? g_anchorRow : row, row);
                g_selRow = row;
            } else {                         // 普通单击：单选
                g_multiSel.clear();
                g_selRow    = row;
                g_anchorRow = row;
            }
        }
        UpdateButtons();
        ListRepaint();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {                 // 双击任务内容 -> 就地编辑
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int row = RowFromY(y);
        if (row >= 0 && row < (int)g_rows.size() && g_rows[row] >= 0) {
            int idx = g_rows[row];
            RECT ir = RowRect(row);
            if (!PtInBoxZone(ir, x) && !DateLabelHit(idx, ir, x) &&
                !ExpandBtnHit(idx, ir, x, y)) {
                int anchorY = RowAnchorY(idx);   // v1.13.1：双击落点行保持原位
                if (CollapseExpandedExcept(idx)) {
                    RefreshListKeepSel();
                    RestoreRowAnchor(idx, anchorY);
                }
                row = RowOfIdx(idx);
                if (row >= 0) {
                    POINT hit = { x, y };    // v1.13.3：把点击点交给编辑器定位光标
                    OpenRowEditor(row, hit);
                    g_selRow = row;
                    UpdateButtons();
                    ListRepaint();
                }
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {                   // 右键任务（待办/已完成）-> 菜单删除
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };   // 屏幕坐标
        if (pt.x != -1 || pt.y != -1) {
            POINT cpt = pt;
            ScreenToClient(h, &cpt);
            int row = RowFromY(cpt.y);
            if (row >= 0 && row < (int)g_rows.size() && g_rows[row] >= 0) {
                CloseRowEditor(true);        // 正在行内编辑则先保存关闭
                g_menuRow = row;
                // v0.16：右键目标不在多选集中 -> 改为单选它（在多选集中 ->
                // 保留多选集，菜单提供批量操作）
                if (!MultiHas(g_rows[row])) {
                    g_multiSel.clear();
                    g_selRow    = row;
                    g_anchorRow = row;
                }
                HMENU menu = CreatePopupMenu();
                if (menu) {
                    if (g_multiSel.size() >= 2) {   // v0.16：多选 -> 批量操作
                        wchar_t lab[80];
                        bool anyDone = false;
                        for (int i : g_multiSel)
                            if (g_tasks[(size_t)i].done) { anyDone = true; break; }
                        std::swprintf(lab, 80, L"完成所选（%d 项）",
                                      (int)g_multiSel.size());
                        AppendMenuW(menu, MF_STRING, IDM_MULTI_DONE, lab);
                        if (anyDone) {
                            std::swprintf(lab, 80, L"还原所选（%d 项）",
                                          (int)g_multiSel.size());
                            AppendMenuW(menu, MF_STRING, IDM_MULTI_UNDONE, lab);
                        }
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        std::swprintf(lab, 80, L"删除所选（%d 项）",
                                      (int)g_multiSel.size());
                        AppendMenuW(menu, MF_STRING, IDM_MULTI_DELETE, lab);
                    } else {
                        AppendMenuW(menu, MF_STRING, IDM_ROW_DELETE, L"删除任务");
                        // v0.15：已完成任务额外提供「修改完成日期」
                        if (g_tasks[(size_t)g_rows[row]].done)
                            AppendMenuW(menu, MF_STRING, IDM_ROW_EDITDONE,
                                        L"修改完成日期");
                    }
                    SetForegroundWindow(g_hwnd);
                    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, pt.x, pt.y,
                                     g_hwnd, nullptr);
                    PostMessageW(g_hwnd, WM_NULL, 0, 0);
                    DestroyMenu(menu);
                }
                return 0;
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (g_sbDrag) {                      // 拖动滚动条：像素级映射滚动
            RECT crc;
            GetClientRect(h, &crc);
            double maxPx = MaxScrollPx();
            int trackLen = crc.bottom - Scale(8);
            int thumbH = (int)((double)g_viewH * trackLen / g_totalH);
            if (thumbH < Scale(28)) thumbH = Scale(28);
            if (thumbH > trackLen) thumbH = trackLen;
            int maxThumb = trackLen - thumbH;
            if (maxThumb > 0 && maxPx > 0) {
                int ty = y - g_sbGrabOff - Scale(4);
                ListScrollTo((double)ty * maxPx / maxThumb, false);
            }
            return 0;
        }
        // 滚动条热区悬停状态（thumb 加深；v0.24 悬停热区唤出自动隐藏的 thumb）
        bool overSb = SbHit(x);
        if (overSb) SbPoke();
        if (overSb != g_sbTrack) {
            g_sbTrack = overSb;
            ListRepaint();
        }
        int  row = -1;
        bool box = false;
        bool hand = false;   // 方框或日期标签（含「＋ 日期」占位）上显示手型光标
        int  line = -1;      // v0.27 整行悬停（任意任务行，含勾选框热区之外）
        if (!overSb) {
            int k = RowFromY(y);
            if (k >= 0 && k < (int)g_rows.size()) {
                if (g_rows[k] >= 0) {
                    line = k;                        // v0.27：整行悬停
                    RECT ir = RowRect(k);
                    if (PtInBoxZone(ir, x)) {
                        row = k;
                        box = true;
                        hand = true;
                    } else if (DateLabelHit(g_rows[k], ir, x)) {
                        hand = true;
                    } else if (ExpandBtnHit(g_rows[k], ir, x, y)) {
                        hand = true;         // 「展开 / 收起」手型光标
                    }
                } else if (g_rows[k] != ROW_FILL) {   // 分组标题「↓ 导出」热区
                    RECT ir = RowRect(k);
                    if (HeaderExportHit(-1 - g_rows[k], ir, x)) hand = true;
                }
            }
        }
        if (row != g_hoverRow || box != g_hoverBox) {
            int old = g_hoverRow;
            g_hoverRow = row;
            g_hoverBox = box;
            RECT ir;
            if (old >= 0 && old < (int)g_rows.size()) {
                ir = RowRect(old);
                InvalidateRect(h, &ir, FALSE);
            }
            if (row >= 0) {
                ir = RowRect(row);
                InvalidateRect(h, &ir, FALSE);
            }
        }
        if (line != g_hoverLine) {           // v0.27：换行时新旧行区域失效重绘
            int old = g_hoverLine;
            g_hoverLine = line;
            RECT ir;
            if (old >= 0 && old < (int)g_rows.size()) {
                ir = RowRect(old);
                InvalidateRect(h, &ir, FALSE);
            }
            if (line >= 0) {
                ir = RowRect(line);
                InvalidateRect(h, &ir, FALSE);
            }
        }
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        if (!g_trackMouse) {
            TRACKMOUSEEVENT te { sizeof(te), TME_LEAVE, h, 0 };
            if (TrackMouseEvent(&te)) g_trackMouse = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_trackMouse = false;
        if (g_sbTrack) { g_sbTrack = false; ListRepaint(); }
        if (g_hoverRow >= 0 && g_hoverRow < (int)g_rows.size()) {
            RECT ir = RowRect(g_hoverRow);
            InvalidateRect(h, &ir, FALSE);
        }
        g_hoverRow = -1;
        g_hoverBox = false;
        if (g_hoverLine >= 0 && g_hoverLine < (int)g_rows.size()) {
            RECT ir = RowRect(g_hoverLine);   // v0.27：整行悬停同步失效
            InvalidateRect(h, &ir, FALSE);
        }
        g_hoverLine = -1;
        return 0;
    case WM_CAPTURECHANGED:
        if (g_sbDrag) { g_sbDrag = false; ListRepaint(); }
        return 0;
    case WM_LBUTTONUP:
        if (g_sbDrag) {
            g_sbDrag = false;
            if (GetCapture() == h) ReleaseCapture();
            ListRepaint();
            return 0;
        }
        return 0;
    case WM_KEYDOWN: {
        int cnt = (int)g_rows.size();
        auto moveSel = [&](int nr) {
            if (nr < 0 || nr >= cnt || nr == g_selRow) return;
            CloseRowEditor(true);
            g_selRow    = nr;
            g_anchorRow = nr;                // v0.16：键盘移动重设批量选锚点
            g_multiSel.clear();              // v0.16：键盘导航回到单选
            ListEnsureVisible(nr);
            UpdateButtons();
            ListRepaint();
        };
        switch (w) {
        case VK_SPACE: {                     // 空格 = 切换完成 / 折叠分组
            if (g_selRow >= 0 && g_selRow < cnt) {
                if (g_rows[g_selRow] == ROW_FILL) return 0;
                if (g_rows[g_selRow] < 0) ToggleGroup(-1 - g_rows[g_selRow]);
                else ToggleTask(g_selRow);
            }
            return 0;
        }
        case VK_UP:
        case VK_LEFT:
            moveSel(g_selRow < 0 ? cnt - 1 : g_selRow - 1);
            return 0;
        case VK_DOWN:
        case VK_RIGHT:
            moveSel(g_selRow < 0 ? 0 : g_selRow + 1);
            return 0;
        case VK_HOME:
            moveSel(cnt > 0 ? 0 : -1);
            return 0;
        case VK_END:
            moveSel(cnt - 1);
            return 0;
        case VK_PRIOR: {                     // PgUp：上移一屏并对齐到新顶行
            if (cnt <= 0) return 0;
            int cy = (int)g_scrollPx - g_viewH;
            if (cy < 0) cy = 0;
            ListScrollTo(cy, true);
            moveSel(RowFromContentY(cy));
            return 0;
        }
        case VK_NEXT: {                      // PgDn：下移一屏并对齐到新顶行
            if (cnt <= 0) return 0;
            int cy = (int)g_scrollPx + g_viewH;
            double maxPx = MaxScrollPx();
            if (cy > (int)maxPx) cy = (int)maxPx;
            ListScrollTo(cy, true);
            moveSel(RowFromContentY(cy));
            return 0;
        }
        case 'A':                            // v0.16：Ctrl+A 全选全部任务
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                g_multiSel.clear();
                for (int r = 0; r < cnt; ++r)
                    if (g_rows[r] >= 0) g_multiSel.push_back(g_rows[r]);
                CloseRowEditor(true);
                UpdateButtons();
                ListRepaint();
                return 0;
            }
            break;
        }
        break;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------- 布局 ------------------------------------------
static void Layout() {
    RECT rc {};
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m       = Scale(14);
    int inputH  = (g_inputH > 0) ? g_inputH : Scale(36);   // v1.12.0 随行数长高
    int topY    = Scale(72);
    int addW    = Scale(96);
    int hintH   = Scale(22);
    int listTop = topY + inputH + hintH + Scale(6);
    // v1.13.12：底栏整体移除，列表直接铺到窗口底部（下留一个 m 的边距）
    int listH   = H - listTop - m;
    if (listH < Scale(80)) listH = Scale(80);

    // v1.13.17：改变输入框尺寸会让 EDIT 重排，插入点可能被清掉。改动前后
    // 保存/恢复一次（只在它持有焦点时），这是所有布局路径的最后一道保险
    DWORD inSel0 = 0, inSel1 = 0;
    bool  inFocus = (g_input && GetFocus() == g_input);
    if (inFocus)
        SendMessageW(g_input, EM_GETSEL, (WPARAM)&inSel0, (LPARAM)&inSel1);
    MoveWindow(g_input,  m, topY, W - m * 2 - addW - Scale(8), inputH, TRUE);
    MoveWindow(g_btnAdd, W - m - addW, topY, addW, inputH, TRUE);
    // v1.13.11：列表内缩 1px，父窗口画圆角卡片描边，列表自身用圆角 region
    // 裁剪 —— 得到真正的圆角白卡，而不是直角白块
    MoveWindow(g_list,   m + 1, listTop + 1, W - m * 2 - 2, listH - 2, TRUE);
    {
        int rad = Scale(12);
        HRGN rgn = CreateRoundRectRgn(0, 0, W - m * 2 - 1, listH - 1,
                                      rad, rad);
        SetWindowRgn(g_list, rgn, TRUE);   // 成功后 region 归系统所有
    }
    CenterInput();
    if (inFocus) {
        int len = (int)SendMessageW(g_input, WM_GETTEXTLENGTH, 0, 0);
        if ((int)inSel0 > len) inSel0 = (DWORD)len;
        if ((int)inSel1 > len) inSel1 = (DWORD)len;
        SendMessageW(g_input, EM_SETSEL, (WPARAM)inSel0, (LPARAM)inSel1);
    }
    // v0.27：输入框外描边矩形（父窗口 WM_PAINT 绘制）。v0.27.1：外扩量
    // 1px → Scale(2) —— 圆角弧线相对外接矩形最大下沉约 0.29×半径
    // （1px 笔宽再 +0.5px），外扩 1px 时弧线探进输入框窗口被其不透明
    // 底色盖掉（四角缺线）；外扩 Scale(2) 后弧线完整落在父窗口区域
    SetRect(&g_inputFrame, m - Scale(2), topY - Scale(2),
            W - m - addW - Scale(8) + Scale(2), topY + inputH + Scale(2));

    // 右上角「更多」：与标题同一视觉高度
    int moreW = Scale(34), moreH = Scale(26);
    MoveWindow(g_btnMore, W - m - moreW, Scale(14), moreW, moreH, TRUE);

    SetRect(&g_listFrame, m - 1, listTop - 1, W - m + 1, listTop + listH + 1);
    SetRect(&g_hintRect, m + Scale(2), topY + inputH + Scale(3),
            W - m, topY + inputH + Scale(3) + Scale(18));
}

// ---------------------------- 绘制 ------------------------------------------
// 分组标题行（grp 0..4 完成 / 5..9 未完成）：v0.15 起两类分组均带折叠三角，
// 未完成分组无「↓ 导出」；空的未完成分组在 RebuildRows 中已隐藏不会到达这里
static void DrawHeaderRow(HDC hdc, const RECT& rc, int grp) {
    bool open = GroupOpen(grp);
    bool unfinished = (grp >= 5);
    // v1.13.11：两类分组标题统一铺浅灰底带 —— 卡片内形成「小节标题」层次，
    // 未完成区原来是纯白，和任务行糊在一起分不出段
    FillSolid(hdc, rc, MixColor(COL_BG, COL_TEXT, 0.03));   // 略深于页面底
    RECT line = { rc.left, rc.top, rc.right, rc.top + Scale(1) };
    FillSolid(hdc, line, COL_BORDER);

    int tri = Scale(8);
    int tx = rc.left + Scale(14);
    int cy = rc.top + ((rc.bottom - rc.top) - tri) / 2;
    POINT pts[3];
    if (open) {
        pts[0] = { tx, cy };
        pts[1] = { tx + tri, cy };
        pts[2] = { tx + tri / 2, cy + tri };
    } else {
        pts[0] = { tx, cy };
        pts[1] = { tx, cy + tri };
        pts[2] = { tx + tri, cy + tri / 2 };
    }
    HBRUSH b = CachedBrush(COL_MUTED);   // v1.9.1：缓存复用，不再即建即删
    HPEN  p = CachedPen(COL_MUTED);
    HGDIOBJ ob = SelectObject(hdc, b);
    HGDIOBJ op = SelectObject(hdc, p);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);

    // v0.22：分组名称与任务数量分开绘制 —— 名称保持分组字体颜色，
    // 数量用浅灰（COL_GRP_CNT）弱化显示，不再与名称同色混排影响阅读
    const wchar_t* gname = (grp >= 5) ? OpenGroupName(grp) : GroupName(grp);
    int cnt = (grp >= 5) ? g_openCount[grp - 5] : g_grpCount[grp];
    wchar_t num[16];
    std::swprintf(num, 16, L"%d", cnt);
    HGDIOBJ of = SelectObject(hdc, unfinished ? g_fontGroupBold : g_fontGroupDone);
    SetBkMode(hdc, TRANSPARENT);
    RECT tr = { tx + tri + Scale(8), rc.top, rc.right - Scale(10), rc.bottom };
    // v1.13.6：未完成 / 已完成分组各自取自己的颜色（此前共用一套）
    SetTextColor(hdc, unfinished ? EffGrpCol() : EffDoneGrpCol());
    DrawTextW(hdc, gname, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SIZE ns {};
    GetTextExtentPoint32W(hdc, gname, (int)wcslen(gname), &ns);
    RECT nr = { tr.left + ns.cx + Scale(7), rc.top, tr.right, rc.bottom };
    if (unfinished) SelectObject(hdc, g_fontGroup);   // v0.25：计数保持常规字重
    SetTextColor(hdc, COL_GRP_CNT);
    DrawTextW(hdc, num, -1, &nr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(hdc, of);

    // v1.10.0 右侧「↓ 导出」：仅完成分组提供，且当分组内有任务时显示，
    // 单击保存为 txt（每任务一行）
    if (grp < 5 && g_grpCount[grp] > 0) {
        HGDIOBJ oe = SelectObject(hdc, g_fontGroupDone);   // v1.13.6：同已完成分组
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, EffDoneGrpCol());
        RECT er = { rc.left, rc.top, rc.right - Scale(10), rc.bottom };
        DrawTextW(hdc, kExportLabel, -1, &er,
                  DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, oe);
    }
}

// v1.13.11：已完成区总标题 —— 收纳五个子分组时唯一可见的一行，
// 整行可点，展开后五个分组重新出现，这一行继续作为总标题留在顶部
static void DrawDoneSummaryRow(HDC hdc, const RECT& rc) {
    FillSolid(hdc, rc, MixColor(COL_BG, COL_TEXT, 0.03));
    RECT line = { rc.left, rc.top, rc.right, rc.top + Scale(1) };
    FillSolid(hdc, line, COL_BORDER);
    int tri = Scale(8);
    int tx  = rc.left + Scale(16);
    int cy  = rc.top + ((rc.bottom - rc.top) - tri) / 2;
    POINT pts[3];
    if (g_doneCollapsed) {                 // 收纳：三角向右
        pts[0] = { tx, cy };
        pts[1] = { tx, cy + tri };
        pts[2] = { tx + tri, cy + tri / 2 };
    } else {                               // 展开：三角向下
        pts[0] = { tx, cy };
        pts[1] = { tx + tri, cy };
        pts[2] = { tx + tri / 2, cy + tri };
    }
    HGDIOBJ ob = SelectObject(hdc, CachedBrush(COL_MUTED));
    HGDIOBJ op = SelectObject(hdc, CachedPen(COL_MUTED));
    Polygon(hdc, pts, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);

    HGDIOBJ of = SelectObject(hdc, g_fontGroupDone);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, EffDoneGrpCol());
    RECT tr = { tx + tri + Scale(8), rc.top, rc.right - Scale(16), rc.bottom };
    const wchar_t* label = L"已完成";
    DrawTextW(hdc, label, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SIZE ns {};
    GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &ns);
    wchar_t num[16];
    std::swprintf(num, 16, L"%d", CountDone());
    RECT nr = { tr.left + ns.cx + Scale(7), rc.top, tr.right, rc.bottom };
    SetTextColor(hdc, COL_GRP_CNT);
    DrawTextW(hdc, num, -1, &nr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(hdc, of);
}

static void DrawTaskRow(HDC hdc, const RECT& rc, const Task& t, bool sel,
                        bool rowHover, bool hoverBox, bool divider, int idx) {
    // v0.28：选中与悬停同几何 —— 都是左右各内缩 2px 的圆角卡片
    //（RoundRect 半径 6px，空笔画纯底色）：选中 = 淡灰选中底、悬停 =
    // 淡灰悬停底（v0.27 悬停为整行铺色，与选中卡片的视觉语言不一致）；
    // 其余 = 白（深色主题下为内容卡色，随色板自动切换）
    if (sel || rowHover) {
        RECT cr = { rc.left + Scale(2), rc.top, rc.right - Scale(2), rc.bottom };
        HGDIOBJ op = SelectObject(hdc, GetStockObject(NULL_PEN));
        HGDIOBJ ob = SelectObject(hdc, CachedBrush(sel ? COL_SEL_BG
                                                       : COL_HOVER_BG));
        RoundRect(hdc, cr.left, cr.top, cr.right, cr.bottom,
                  Scale(6), Scale(6));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
    } else {
        FillSolid(hdc, rc, COL_WHITE);
    }

    // 勾选框（悬停时高亮：未完成填充淡灰、边框变蓝；已完成边框加深）；
    // v0.27：直角 Rectangle 改为圆角（半径 5px 按 DPI 缩放），对勾不变
    int box = Scale(18);
    int bx  = rc.left + Scale(12);
    // v1.13.9：勾选框在整行内垂直居中，编辑态与展开态完全一致 —— 之前给
    // 编辑行单独做了「顶部对齐」，双击后勾选框会突然跳到行顶，看起来像
    // 换了另一个控件；编辑态本来就该是「展开态 + 一个光标」，其余不动
    int byy = rc.top + ((rc.bottom - rc.top) - box) / 2;
    RECT br = { bx, byy, bx + box, byy + box };
    COLORREF fc = t.done ? COL_ACCENT : (hoverBox ? COL_SEL_BG : COL_WHITE);
    COLORREF bc = t.done ? (hoverBox ? COL_HIGHLIGHT : COL_ACCENT)
                         : (hoverBox ? COL_ACCENT : COL_BOX_BD);
    HBRUSH fill = CachedBrush(fc);       // v1.9.1：缓存复用，不再即建即删
    HPEN   pen  = CachedPen(bc);
    HGDIOBJ of = SelectObject(hdc, fill);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, br.left, br.top, br.right, br.bottom, Scale(5), Scale(5));
    SelectObject(hdc, of);
    SelectObject(hdc, op);

    if (t.done) {
        HGDIOBJ oc = SelectObject(hdc, g_penCheck);   // v1.9.1：预创建复用
        MoveToEx(hdc, br.left + MulDiv(box, 24, 100),
                       br.top  + MulDiv(box, 53, 100), nullptr);
        POINT pts[2] = {
            { br.left + MulDiv(box, 42, 100), br.top + MulDiv(box, 71, 100) },
            { br.left + MulDiv(box, 77, 100), br.top + MulDiv(box, 31, 100) },
        };
        PolylineTo(hdc, pts, 2);
        SelectObject(hdc, oc);
    }

    SetBkMode(hdc, TRANSPARENT);

    // 右侧日期时间标签（无日期且未完成时为「＋ 日期」占位）
    bool od = false;
    if (t.due) FormatWhen(t.due, t.hasTime, t.done, &od);
    std::wstring when = WhenLabelText(t);
    int whenW = 0;
    if (!when.empty()) {
        HGDIOBJ ow = SelectObject(hdc, t.done ? g_fontSub : g_fontDate);   // v0.26 与绘制同源
        SIZE sz {};
        GetTextExtentPoint32W(hdc, when.c_str(), (int)when.size(), &sz);
        whenW = sz.cx;
        SelectObject(hdc, ow);
    }
    int textRight = rc.right - Scale(12) - RowGutter();
    if (whenW) textRight -= whenW + Scale(12);

    HGDIOBJ ot = SelectObject(hdc, t.done ? g_fontDone : g_font);
    SetTextColor(hdc, t.done ? COL_DONE : EffTaskCol());   // v1.13.0 可设置 + v0.27 随主题适配
    int textLeft = br.right + Scale(12);

    // v1.12.1：按折行结果分流 —— 无 \n 的长文本（如划词添加的一段话）
    // 折行后超过一行时同样走多行显示、随窗口宽度自动换行
    std::vector<std::wstring> lines = WrapTaskText(hdc, t.text,
                                                   textRight - textLeft);
    int lineH = TaskLineHeight(hdc);
    int total = (int)lines.size();
    if (total <= 1) {
        // 单行任务：单行 + 省略号显示。v0.27.1：不再用 DT_VCENTER 居中于
        // 整行 —— 改为与多行分支同机制：取规范行高 lineH，行矩形顶边
        // ty = 行顶 + (行高-lineH)/2，DrawText 顶对齐绘制。行内编辑框
        // （RowEditBoxRect 单行分支）窗口顶边取同一 ty，两态字形由同一
        // 绘制机制在同一矩形内完成，双击进入编辑不再有垂直跳动
        //（此前 DT_VCENTER 与 EDIT 顶部绘制的度量差导致文字上移，
        // 公式补偿跨字体/度量不可靠，故改为机制同源）
        int ty = rc.top + ((rc.bottom - rc.top) - lineH) / 2;
        if (ty < rc.top) ty = rc.top;
        RECT tr = { textLeft, ty, textRight, ty + lineH };
        DrawTextW(hdc, t.text.c_str(), -1, &tr,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        // 多行任务：自动换行显示全部文本；未展开最多 5 行，
        // 超出时末行省略号截断，右下角出现「展开 / 收起」按钮
        bool truncated = (!t.expanded && total > kTaskMaxLines);
        int vis = truncated ? kTaskMaxLines : total;
        // v0.28.1：编辑中的行不再绘制任务文本 —— 编辑框行数减少时（全选
        // 删除 / 逐行回删）行高与框高按编辑态公式立即回缩，任务文本却要
        // 等保存才更新，若仍按原文绘制，未被缩小后的框盖住的区域会露出
        // 旧文字（v0.23 只堵了封顶溢画，未封顶的收缩场景同样会漏；列表
        // 末行下方无后续行覆盖时最明显）。编辑框是不透明子窗口且底色与
        // 选中卡片一致，其下方的原文本从来不可见，跳过绘制对常态外观零
        // 影响；框外的日期标签仍由函数末尾绘制
        if (idx == g_rowEditIdx && g_rowEdit) vis = 0;
        // v0.17：编辑中的行不画「展开 / 收起」按钮（行高公式也不保留按钮行，
        // 编辑框覆盖全部内容）
        bool hasBtn = (idx != g_rowEditIdx) && total > kTaskMaxLines;
        // v1.13.2：文本起点固定为「行顶 + Scale(10)」，与 RowEditBoxRect 的
        // 编辑框顶边同一条公式。此前用垂直居中（(innerH - vis*lineH)/2），
        // 行高并入 4px 光标余量后居中结果变成 +12，与编辑框 +10 差 2px，
        // 双击时整段文字会往下跳 2px —— 固定顶边后两态完全同位。
        int top = rc.top + Scale(10);
        for (int i = 0; i < vis; ++i) {
            RECT lr = { textLeft, top + i * lineH,
                        textRight, top + (i + 1) * lineH };
            if (truncated && i == vis - 1) {
                // 末行截断：逐字回退到能容纳「…」为止
                std::wstring s = lines[(size_t)i];
                for (;;) {
                    std::wstring cand = s + L"…";
                    SIZE sz {};
                    GetTextExtentPoint32W(hdc, cand.c_str(),
                                          (int)cand.size(), &sz);
                    if (sz.cx <= textRight - textLeft || s.empty()) {
                        DrawTextW(hdc, cand.c_str(), -1, &lr,
                                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                        break;
                    }
                    s.pop_back();
                }
            } else {
                DrawTextW(hdc, lines[(size_t)i].c_str(), -1, &lr,
                          DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        if (hasBtn) {
            // 「展开 / 收起」按钮（v0.25：颜色与分组标题保持一致；v0.27 同源）
            const wchar_t* label = t.expanded ? L"\u25B4 收起" : L"\u25BE 展开";
            SetTextColor(hdc, EffGrpCol());
            SIZE sz {};
            GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &sz);
            // v1.13.5：按钮在行底右下角（原位置）；展开后的长任务行底可能
            // 在视口之外，此时吸附到视口底部 —— 否则「收起」根本点不到
            int by = rc.bottom - Scale(6) - Scale(17);
            bool floating = false;
            if (g_viewH > Scale(40) && by + Scale(17) > g_viewH - Scale(4)) {
                by = g_viewH - Scale(4) - Scale(17);
                floating = true;
            }
            if (by < rc.top + Scale(4)) by = rc.top + Scale(4);
            if (floating) {
                // v1.13.13：吸附时先铺一块不透明胶囊底 —— 否则按钮文字直接
                // 压在任务正文上（末行被视口裁切时最明显），按钮看不清、正文
                // 也被搅乱。胶囊比文字略大，点到边缘也算命中
                RECT bg = { textRight - sz.cx - Scale(8), by - Scale(4),
                            textRight + Scale(6), by + Scale(17) + Scale(4) };
                HGDIOBJ ob2 = SelectObject(hdc, CachedBrush(COL_WHITE));
                HGDIOBJ op2 = SelectObject(hdc, CachedPen(COL_BORDER));
                RoundRect(hdc, bg.left, bg.top, bg.right, bg.bottom,
                          Scale(9), Scale(9));
                SelectObject(hdc, ob2);
                SelectObject(hdc, op2);
            }
            RECT er = { textRight - sz.cx, by, textRight, by + Scale(17) };
            DrawTextW(hdc, label, -1, &er,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
    }
    SelectObject(hdc, ot);

    // 日期时间
    if (!when.empty()) {
        // v0.26：未完成任务（含逾期、今天、「＋ 日期」占位）的日期时间字体
        // 大小与颜色均可在设置中自定义（不再绑定分组标题颜色）；
        // 已完成保持固定小字号 + 完成灰
        HGDIOBJ ow = SelectObject(hdc, t.done ? g_fontSub : g_fontDate);
        SetTextColor(hdc, t.done ? COL_DONE : EffDtCol());   // v0.27 随主题适配
        // v1.13.9：日期同样保持整行垂直居中（编辑态与展开态一致，不跳动）
        RECT wr = { rc.right - Scale(12) - RowGutter() - whenW, rc.top,
                    rc.right - Scale(6) - RowGutter(), rc.bottom };
        DrawTextW(hdc, when.c_str(), -1, &wr,
                  DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(hdc, ow);
    }

    // v1.12.2 任务行之间的灰色分隔线：仅当下一行也是任务行时绘制
    //（与分组标题行顶线同色 COL_BORDER；v0.25 选中行为浅灰底、不再包边；
    // v0.28 悬停行与选中行同规则 —— 卡片行的底部分隔线一并隐藏）
    if (divider && !sel && !rowHover) {
        RECT sr = { rc.left, rc.bottom - Scale(1), rc.right, rc.bottom };
        FillSolid(hdc, sr, COL_BORDER);
    }
}

// ---------------------------- 主按钮悬停跟踪（v1.13.10） ---------------------
static LRESULT CALLBACK BtnAddProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEMOVE && !g_btnAddHot) {
        g_btnAddHot = true;
        TRACKMOUSEEVENT tme { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, TRUE);
    } else if (m == WM_MOUSELEAVE) {
        g_btnAddHot = false;
        InvalidateRect(h, nullptr, TRUE);
    }
    return CallWindowProcW(g_oldBtnAddProc, h, m, w, l);
}

// v1.13.11：右上角「⋯」按钮的悬停跟踪
static LRESULT CALLBACK BtnMoreProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEMOVE && !g_btnMoreHot) {
        g_btnMoreHot = true;
        TRACKMOUSEEVENT tme { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, TRUE);
    } else if (m == WM_MOUSELEAVE) {
        g_btnMoreHot = false;
        InvalidateRect(h, nullptr, TRUE);
    }
    return CallWindowProcW(g_oldBtnMoreProc, h, m, w, l);
}

// ---------------------------- 窗口过程 --------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCT)lParam)->hInstance;
        // 多行 EDIT + EM_SETRECT 实现光标垂直居中；ES_AUTOVSCROLL：粘贴多行
        // 文本时自动换行（v1.12.0），超过 4 行内部滚动；
        // ES_WANTRETURN（v0.16）：Shift+Enter 由默认过程插入换行
        // v0.27：去掉 WS_EX_CLIENTEDGE 经典下凹边框（父窗口画 1px 圆角描边）
        g_input = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_INPUT, hi, nullptr);
        SendMessageW(g_input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(8), Scale(8)));   // 去边框后补内边距
        // v0.27：五个按钮全部改为自绘（BS_OWNERDRAW）——「＋ 添加」为强调色
        // 实心圆角主按钮，其余为幽灵描边按钮，绘制在 WM_DRAWITEM
        g_btnAdd = CreateWindowExW(0, L"BUTTON", L"\uFF0B 添加",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_ADD, hi, nullptr);
        // v1.13.0：列表改为完全自绘子窗口 TicklyList（像素级平滑滚动、
        // 悬浮滚动条、双缓冲无闪烁；不再使用 ListBox）
        g_list = CreateWindowExW(0, L"TicklyList", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LIST, hi, nullptr);
        g_btnMore = CreateWindowExW(0, L"BUTTON", L"⋯",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_MORE, hi, nullptr);

        SendMessageW(g_input,     WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_list,      WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_btnAdd,    WM_SETFONT, (WPARAM)g_fontSub, TRUE);
        g_oldBtnAddProc = (WNDPROC)SetWindowLongPtrW(g_btnAdd, GWLP_WNDPROC,
                                                     (LONG_PTR)BtnAddProc);
        SendMessageW(g_btnMore,   WM_SETFONT, (WPARAM)g_fontSub, TRUE);
        g_oldBtnMoreProc = (WNDPROC)SetWindowLongPtrW(g_btnMore, GWLP_WNDPROC,
                                                      (LONG_PTR)BtnMoreProc);

        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC,
                                                   (LONG_PTR)EditProc);
        SetTimer(hwnd, 1, 15000, nullptr);   // 周期检查任务提醒
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        // 托盘常驻三步：① 预写（上次运行遗留的设置子键先置 IsPromoted=1，
        // Explorer 添加图标那一刻会读取）② 添加图标 ③ 兑底（未生效则写入
        // + 重加 + 定时重试，覆盖首启子键异步建立的情况）
        g_trayAddTries = 0;
        g_aliveMiss    = 0;
        g_trayRollback = false;
        int pre = PromoteTrayIcon();         // ① 预写，记住结果用于验证期判定
        AddTrayIcon();                       // ② 任务栏托盘图标
        SetTimer(hwnd, 5, 5000, nullptr);    // ④ 心跳保活：图标丢了 10 秒内自动恢复
        if (!g_nidAdded) EnsureTrayIcon(hwnd);
        else {
            g_promoteTries = 0;
            if (pre != TRAY_PROMOTE_NONE) {
                g_trayPromoted = true;       // 预写生效（或本就常驻）
                BeginTrayAliveCheck();       // 心跳确认常驻无副作用
            }
            ApplyTrayPromotion(hwnd);        // ③ 图标常驻右下角，不被收进「^」折叠区
        }
        Layout();
        ApplyTheme();        // v0.27：窗口创建即应用 DWM 外壳与主题色板
                             //（此时尚未 LoadTasks，按默认「跟随系统」解析；
                             //  加载持久化设置后 wWinMain 会再调一次）
        return 0;
    }

    case WM_DPICHANGED: {
        // v1.13.24：跨显示器拖动或系统缩放比例变化。用新 DPI 重建字体/画笔，
        // 再按系统建议矩形改窗口大小 —— 否则窗口尺寸与字号仍是旧比例
        UINT nd = HIWORD(wParam);          // Y 轴 DPI（X/Y 通常一致）
        if (nd) g_dpi = nd;
        if (g_list) {                      // 创建期（子窗口未就绪）只调整尺寸
            CreateUiResources();           // 标题/气泡/提醒字体 + 对勾画笔
            ApplyFontSettings();           // 任务/分组/日期字体 + 行高 + 布局
        }
        const RECT* pr = (const RECT*)lParam;
        if (pr)
            SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_SIZE:
        CloseRowEditor(true);            // 窗口尺寸变化，行位置改变
        Layout();
        // v1.13.0：填塞行/行高/滚动钳制由列表窗口自身 WM_SIZE 处理
        if (wParam != SIZE_MINIMIZED && !g_wpTimer)
            g_wpTimer = SetTimer(hwnd, 2, 1000, nullptr);   // 1 秒后落盘新尺寸
        return 0;

    case WM_EXITSIZEMOVE:                // 拖动/缩放结束 -> 立即保存窗口位置大小
        if (g_wpTimer) { KillTimer(hwnd, 2); g_wpTimer = 0; }
        SaveTasks();                     //（关机/重启/强杀时 WM_DESTROY 不会执行，
        return 0;                        //  仅靠退出时保存会丢失最后一次调整）

    case WM_ENDSESSION:                  // Windows 关机/重启 -> 尽快落盘一次
        if (wParam) SaveTasks();
        return 0;

    case WM_SETFOCUS:
        SetFocus(g_input);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = Scale(440);
        mmi->ptMinTrackSize.y = Scale(560);
        return 0;
    }

    case WM_CLOSE:                       // 点 X：按设置决定托盘后台或真正退出
        if (g_closeToTray) HideToTray(); //   勾选（默认）：缩到托盘，提醒照常弹出
        else DestroyWindow(hwnd);        //   取消勾选：直接退出
        return 0;

    case WM_TRAY: {                      // 托盘图标交互
        if (lParam == WM_LBUTTONUP) {
            ShowMainWindow();            // 左键：显示/还原主窗口
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (menu) {
                AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"显示主窗口");
                AppendMenuW(menu, MF_STRING, IDM_TRAY_SET,  L"设置…");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出");
                SetForegroundWindow(hwnd);   // 保证点击菜单外能关闭菜单
                TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
        }
        return 0;
    }

    case WM_OPENDATE:                    // 单击日期标签 -> 修改日期时间
        if ((int)wParam >= 0 && (int)wParam < (int)g_tasks.size())
            OpenDateEditor((int)wParam);
        return 0;

    case WM_HOTKEY:                      // v1.12.0 全局快捷键
        if (wParam == HK_ID_SHOW)          ShowMainWindow();
        else if (wParam == HK_ID_CAP)      CaptureSelectedToTask();
        return 0;

    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IDOK)                            { AddFromInput();   return 0; }
        if (id == IDCANCEL)                        { SetWindowTextW(g_input, L""); SetFocus(g_input); return 0; }
        if (id == IDC_BTN_ADD)                     { AddFromInput();   return 0; }
        if (id == IDC_BTN_MORE) {                   // v1.13.11：右上角「⋯」菜单
            HMENU menu = CreatePopupMenu();
            if (menu) {
                int done = CountDone();
                wchar_t lab[64];
                AppendMenuW(menu, MF_STRING, IDM_APP_SETTINGS, L"设置…");
                std::swprintf(lab, 64, L"清除已完成（%d）", done);
                AppendMenuW(menu, MF_STRING | (done > 0 ? MF_ENABLED : MF_GRAYED),
                            IDM_APP_CLEAR, lab);
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出 Tickly");
                RECT br {};
                GetWindowRect(g_btnMore, &br);
                SetForegroundWindow(hwnd);
                TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_TOPALIGN,
                                 br.left, br.bottom, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
            return 0;
        }
        if (id == IDM_APP_CLEAR) {                  // 二次确认，防止误删
            int done = CountDone();
            wchar_t msg[128];
            std::swprintf(msg, 128,
                L"确定要清除全部 %d 项已完成的任务吗？\n清除后无法恢复。", done);
            if (done > 0 &&
                MessageBoxW(hwnd, msg, L"清除已完成",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                ClearDone();
            return 0;
        }
        if (id == IDM_APP_SETTINGS)                { OpenSettings();   return 0; }
        if (id == IDM_TRAY_SHOW)                   { ShowMainWindow(); return 0; }
        if (id == IDM_TRAY_SET)                    { ShowMainWindow(); OpenSettings(); return 0; }
        if (id == IDM_TRAY_EXIT)                   { DestroyWindow(hwnd); return 0; }
        if (id == IDM_ROW_DELETE)                  { DeleteTaskAt(g_menuRow); g_menuRow = -1; return 0; }
        if (id == IDM_MULTI_DONE)                  { MultiSetDone(g_multiSel, true);  return 0; }   // v0.16
        if (id == IDM_MULTI_UNDONE)                { MultiSetDone(g_multiSel, false); return 0; }   // v0.16
        if (id == IDM_MULTI_DELETE)                { MultiDelete(g_multiSel);         return 0; }   // v0.16
        if (id == IDM_ROW_EDITDONE) {              // v0.15：修改完成日期
            if (g_menuRow >= 0 && g_menuRow < (int)g_rows.size() &&
                g_rows[g_menuRow] >= 0)
                OpenDoneDateEditor(g_rows[g_menuRow]);
            g_menuRow = -1;
            return 0;
        }
        if (id == IDC_INPUT && code == EN_CHANGE)  {
            // v1.13.16：EN_CHANGE 是 EDIT 在「处理输入的过程中」同步发给父窗口的。
            // 此刻若对同一个控件发 EM_SETRECT / EM_SETSEL，等于在它插入文字的
            // 中途改写它的选区 —— 中文输入法第二段上屏时（文本变长、行数或格式
            // 矩形随之变化）会触发选区恢复，把插入点顶回组合串起点，表现为
            // 「『下午』一上屏光标就跑到前面」。改为投递消息，等 EDIT 把这次输入
            // 彻底处理完、消息循环空闲时再做布局与提示更新
            PostMessageW(hwnd, WM_INPUTCHANGED, 0, 0);
            return 0;
        }
        break;
    }

    case WM_INPUTCHANGED: {              // v1.13.16：输入框内容变化的延迟处理
        UpdateInputHint();
        UpdateInputSize();
        InvalidateRect(g_input, nullptr, TRUE);   // 占位符随空/非空切换
        return 0;
    }

    case WM_DRAWITEM: {                  // v0.27：底部按钮自绘
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis || dis->CtlType != ODT_BUTTON) break;
        bool sel = (dis->itemState & ODS_SELECTED) != 0;
        bool dis_ = (dis->itemState & ODS_DISABLED) != 0;
        bool primary = (dis->CtlID == IDC_BTN_ADD);   // 主操作 = 最高频的「＋ 添加」
        if (dis->CtlID == IDC_BTN_MORE) {             // v1.13.11：右上角「⋯」
            RECT r = dis->rcItem;
            if (g_btnMoreHot) {                       // 悬停：浅灰圆角底
                HGDIOBJ op = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                HGDIOBJ ob = SelectObject(dis->hDC, CachedBrush(COL_HOVER_BG));
                RoundRect(dis->hDC, r.left, r.top, r.right, r.bottom,
                          Scale(6), Scale(6));
                SelectObject(dis->hDC, ob);
                SelectObject(dis->hDC, op);
            }
            int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            int rr = Scale(2);
            if (rr < 1) rr = 1;
            HGDIOBJ op = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
            HGDIOBJ ob = SelectObject(dis->hDC,
                                      CachedBrush(g_btnMoreHot ? COL_TEXT : COL_MUTED));
            for (int i = -1; i <= 1; ++i)
                Ellipse(dis->hDC, cx + i * Scale(7) - rr, cy - rr,
                        cx + i * Scale(7) + rr, cy + rr);
            SelectObject(dis->hDC, ob);
            SelectObject(dis->hDC, op);
            return TRUE;
        }
        COLORREF bg, tx, bd;
        if (primary) {
            // v1.13.10：主按钮改用低饱和柔蓝 —— 原来直接铺 COL_ACCENT(#007AFF)，
            // 大面积纯高饱和蓝在浅灰底上很扎眼；浅色降到 #3D7CE0（饱和度约
            // 73%，明度略降），深色沿用 Apple 深色强调 #0A84FF（暗底不刺眼）
            COLORREF base = g_themeDark ? RGB(10, 132, 255) : RGB(61, 124, 224);
            if (g_btnAddHot && !sel) base = DarkenColor(base, 6);   // 悬停略沉
            bg = sel ? DarkenColor(base, 12) : base;
            tx = COL_WHITE;
            bd = bg;
        } else {                             // 幽灵按钮：背景色底 + 1px 描边 + 正文色
            bg = sel ? DarkenColor(COL_BG, 10) : COL_BG;
            tx = dis_ ? COL_MUTED : EffTaskCol();
            bd = (dis_ || sel) ? COL_BOX_BD : COL_BORDER;
        }
        if (dis_ && !primary) bg = COL_BG;   // 禁用态不加深
        HGDIOBJ op = SelectObject(dis->hDC, CachedPen(bd));
        HGDIOBJ ob = SelectObject(dis->hDC, CachedBrush(bg));
        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                  dis->rcItem.right, dis->rcItem.bottom, Scale(6), Scale(6));
        SelectObject(dis->hDC, ob);
        SelectObject(dis->hDC, op);
        if (primary && (dis->itemState & ODS_FOCUS)) {   // 键盘焦点：内圈描边可见
            HGDIOBJ op2 = SelectObject(dis->hDC, CachedPen(DarkenColor(bg, 30)));
            HGDIOBJ ob2 = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            RoundRect(dis->hDC, dis->rcItem.left + 1, dis->rcItem.top + 1,
                      dis->rcItem.right - 1, dis->rcItem.bottom - 1,
                      Scale(5), Scale(5));
            SelectObject(dis->hDC, ob2);
            SelectObject(dis->hDC, op2);
        }
        wchar_t txt[64] {};
        GetWindowTextW(dis->hwndItem, txt, 64);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, tx);
        HFONT bf = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
        HGDIOBJ of = SelectObject(dis->hDC, bf ? (HGDIOBJ)bf
                                               : GetStockObject(DEFAULT_GUI_FONT));
        RECT tr2 = dis->rcItem;
        DrawTextW(dis->hDC, txt, -1, &tr2,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dis->hDC, of);
        return TRUE;
    }

    case WM_CTLCOLOREDIT: {              // v0.27：主输入框底色/文字色随主题
        HDC hdc = (HDC)wParam;           //（行内编辑框由列表窗口 WM_CTLCOLOREDIT 处理）
        if (g_input && (HWND)lParam == g_input) {
            SetBkColor(hdc, COL_WHITE);
            SetTextColor(hdc, EffTaskCol());
            return (LRESULT)CachedBrush(COL_WHITE);
        }
        break;
    }

    case WM_SETTINGCHANGE:               // v0.27：跟随系统模式下系统深浅色切换
        if (g_themeMode == 2 && lParam &&
            lstrcmpiW((LPCWSTR)lParam, L"ImmersiveColorSet") == 0)
            ApplyTheme();
        return 0;

    case WM_TIMER:
        if (wParam == 1) {
            CheckReminders();
        } else if (wParam == 2) {        // 延时落盘窗口尺寸（贴靠/最大化等）
            KillTimer(hwnd, 2);
            g_wpTimer = 0;
            SaveTasks();
        } else if (wParam == 3) {        // 托盘图标提升重试（子键异步建立）
            KillTimer(hwnd, 3);
            ApplyTrayPromotion(hwnd);    // 成功或重试耗尽后自动停表
        } else if (wParam == 4) {        // 托盘图标添加失败重试
            KillTimer(hwnd, 4);
            EnsureTrayIcon(hwnd);
            if (g_nidAdded) {
                g_promoteTries = 0;
                if (g_trayPromoted && !g_trayRollback)
                    BeginTrayAliveCheck();   // 常驻图标刚重加：验证期确认
                ApplyTrayPromotion(hwnd);
            }
        } else if (wParam == 5) {        // 托盘心跳保活（v1.9.1 自适应频率）：
                                         // 图标可以被系统折叠，但绝不能消失
            // 节奏自适应：验证期/异常恢复期 5 秒一拍快速收敛；连续 6 拍
            // 一切正常后降到 30 秒低频兜底（每分钟仅 2 次微秒级 IPC 探测，
            // 资源占用几乎为零）。任何异常立即回到 5 秒快拍。
            bool fast = !g_nidAdded || g_trayAliveCheck || g_aliveMiss > 0;
            if (fast) {
                g_hbSteady = 0;
                if (g_hbInterval != 5000) {
                    g_hbInterval = 5000;
                    SetTimer(hwnd, 5, 5000, nullptr);   // 同 ID 重调 = 改间隔
                }
            } else if (++g_hbSteady >= 6 && g_hbInterval != 30000) {
                g_hbInterval = 30000;
                SetTimer(hwnd, 5, 30000, nullptr);
            }
            if (!g_nidAdded) {           // 图标不在：直接补加，下轮继续确认
                AddTrayIcon();
                return 0;
            }
            if (!TrayIconAlive()) {      // NIM_MODIFY 失败 = 图标已被系统移除
                if (++g_aliveMiss < 2) return 0;  // 单次失败可能是瞬时抖动
                g_aliveMiss = 0;
                if (g_trayAliveCheck) {  // 提升验证期内丢失：该系统写
                    RollbackTrayPromotion();      // IsPromoted 反而丢图标，
                    g_trayRollback   = true;      // 回滚为系统默认（折叠但
                    g_trayAliveCheck = false;     // 一定显示）
                    g_aliveOk        = 0;
                } else {                 // 普通丢失：直接恢复
                    RemoveTrayIcon();
                    AddTrayIcon();
                }
                if (!g_nidAdded) EnsureTrayIcon(hwnd);
                return 0;
            }
            g_aliveMiss = 0;
            if (g_trayAliveCheck && ++g_aliveOk >= 2) {  // 连续 2 拍存活：
                g_trayAliveCheck = false;                // 提升确认无害
                g_aliveOk        = 0;
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;   // v0.21：背景由 WM_PAINT 统一填充（COL_BG 全覆盖），
                    // 省一遍整窗擦底 —— 点击/展开时不再整个界面闪一下
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc {};
        GetClientRect(hwnd, &rc);
        FillSolid(hdc, rc, COL_BG);

        int m = g_listFrame.left + 1;

        RECT accent = { m, Scale(18), m + Scale(5), Scale(18) + Scale(26) };
        FillSolid(hdc, accent, COL_ACCENT);

        // v0.27：输入框外 1px 圆角描边 —— 聚焦=强调色，否则=边框色
        //（输入框已去 CLIENTEDGE；EN_SETFOCUS/EN_KILLFOCUS 时重绘）
        // v0.27.1：圆角半径 6→4（外接矩形下沉量随半径减小，配合外扩
        // Scale(2) 保证四角弧线不被输入框底色盖掉；4px 亦为 WinUI 标准）
        if (g_input) {
            COLORREF ibc = (GetFocus() == g_input) ? COL_ACCENT : COL_BORDER;
            HGDIOBJ op = SelectObject(hdc, CachedPen(ibc));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, g_inputFrame.left, g_inputFrame.top,
                      g_inputFrame.right, g_inputFrame.bottom, Scale(4), Scale(4));
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
        }

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldF = (HFONT)SelectObject(hdc, g_fontTitle);
        SetTextColor(hdc, COL_TEXT);
        // v1.13.11：标题右侧留出「⋯」按钮的位置
        RECT rt = { m + Scale(15), Scale(16), rc.right - m - Scale(44),
                    Scale(16) + Scale(30) };
        DrawTextW(hdc, L"Tickly", -1, &rt,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // v1.13.11：副标题从「五条操作说明」改为状态摘要 —— 操作提示本来就
        // 没人读，占着一整行；这里改为一眼能看懂的待办概况
        SelectObject(hdc, g_fontSub);
        SetTextColor(hdc, COL_MUTED);
        RECT st = { m + Scale(15), Scale(46), rc.right - m - Scale(44),
                    Scale(46) + Scale(18) };
        int openCnt = 0, todayCnt = 0;
        long long tm0 = TodayMidnight();
        for (const Task& t : g_tasks) {
            if (t.done) continue;
            ++openCnt;
            if (t.due && t.due < tm0 + 86400) ++todayCnt;
        }
        std::wstring sub;
        if (openCnt == 0)
            sub = L"还没有待办 · 在上方输入后回车添加";
        else if (todayCnt > 0)
            sub = std::to_wstring(openCnt) + L" 项待办 · 今天 " +
                  std::to_wstring(todayCnt) + L" 项";
        else
            sub = std::to_wstring(openCnt) + L" 项待办";
        DrawTextW(hdc, sub.c_str(), -1, &st,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // 日期时间实时识别提示（v0.21：UpdateInputHint 已生成完整文案，
        // 这里原样绘制 —— 原来再包一层「识别到：…（回车添加…）」重复渲染）
        if (!g_hintText.empty()) {
            SelectObject(hdc, g_fontSub);
            SetTextColor(hdc, COL_HIGHLIGHT);
            DrawTextW(hdc, g_hintText.c_str(), -1, &g_hintRect,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }

        // v1.13.11：列表外框改为圆角描边（与列表 region 同曲率）
        {
            HGDIOBJ op = SelectObject(hdc, CachedPen(COL_BORDER));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, g_listFrame.left, g_listFrame.top,
                      g_listFrame.right, g_listFrame.bottom,
                      Scale(24), Scale(24));
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
        }

        SelectObject(hdc, oldF);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        RemoveTrayIcon();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 3);
        KillTimer(hwnd, 4);
        KillTimer(hwnd, 5);   // 托盘心跳保活
        if (g_wpTimer) { KillTimer(hwnd, 2); g_wpTimer = 0; }
        UnregisterHotKey(hwnd, HK_ID_SHOW);   // v1.12.0 全局快捷键
        UnregisterHotKey(hwnd, HK_ID_CAP);
        if (g_hwndToast) DestroyWindow(g_hwndToast);
        SaveTasks();
        if (g_fontTitle)   DeleteObject(g_fontTitle);
        if (g_fontSub)     DeleteObject(g_fontSub);
        if (g_fontGroup)   DeleteObject(g_fontGroup);
        if (g_fontGroupBold) DeleteObject(g_fontGroupBold);
        if (g_fontGroupDone) DeleteObject(g_fontGroupDone);
        if (g_fontDate)    DeleteObject(g_fontDate);
        if (g_font)        DeleteObject(g_font);
        if (g_fontDone)    DeleteObject(g_fontDone);
        if (g_fontRemindT) DeleteObject(g_fontRemindT);
        if (g_fontRemindB) DeleteObject(g_fontRemindB);
        if (g_fontToastT)  DeleteObject(g_fontToastT);
        if (g_fontToastB)  DeleteObject(g_fontToastB);
        if (g_brWhite)   DeleteObject(g_brWhite);
        if (g_brBg)      DeleteObject(g_brBg);    // v0.27 窗口类背景刷
        if (g_penCheck)  DeleteObject(g_penCheck);
        PostQuitMessage(0);
        return 0;

    default:
        // Explorer 重启后托盘图标会被移除，重新添加并重新提升
        if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
            RemoveTrayIcon();
            g_trayAddTries = 0;
            AddTrayIcon();
            if (!g_nidAdded) EnsureTrayIcon(hwnd);
            else {
                g_promoteTries = 0;
                if (g_trayPromoted && !g_trayRollback)
                    BeginTrayAliveCheck();   // 新 Explorer 会重读 IsPromoted
                ApplyTrayPromotion(hwnd);
            }
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------- DPI 感知 --------------------------------------
// v1.13.24：进程必须声明 DPI 感知，否则在 125%/150%/175% 这类非 100% 缩放的
// 屏幕上，Windows 会先把整个窗口按 96 DPI 画好再做整幅位图拉伸，文字边界
// 被插值模糊 —— 这就是「打包版文字发虚」的根因（GDI 字体本身没有问题）。
// 正常路径由内嵌清单 app.manifest 声明（PerMonitorV2）；下面这段是运行时兜底，
// 清单丢失/被裁剪时仍能拿到清晰文字。
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN_SetDpiCtx)(HANDLE);
        auto setCtx = (PFN_SetDpiCtx)GetProcAddress(u32,
                                       "SetProcessDpiAwarenessContext");
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4，老版 PM = -3
        if (setCtx && setCtx((HANDLE)(LONG_PTR)-4)) return;
        if (setCtx && setCtx((HANDLE)(LONG_PTR)-3)) return;
    }
    SetProcessDPIAware();   // Win7+ 系统级感知；清单已生效时调用失败，无害
}

// ---------------------------- 入口 ------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // v1.13.24：必须在任何窗口/DC 创建之前声明 DPI 感知
    EnableDpiAwareness();

    // 单实例：同一时刻只允许一个 Tickly 进程。重复双击 exe 时不再启动
    // 第二个副本，而是直接唤起已运行的窗口（还原/显示并置前），同微信/QQ
    HANDLE hMutex = CreateMutexW(nullptr, TRUE,
                                 L"Local\\Tickly_8F3C_SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"TicklyClass", nullptr);
        if (prev) {
            if (IsIconic(prev))              ShowWindow(prev, SW_RESTORE);
            else if (!IsWindowVisible(prev)) ShowWindow(prev, SW_SHOW);
            SetWindowPos(prev, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(prev);
        }
        CloseHandle(hMutex);
        return 0;                            // 第二实例直接退出
    }

    HDC sdc = GetDC(nullptr);
    g_dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);

    INITCOMMONCONTROLSEX icc { sizeof(icc), ICC_DATE_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);          // 日期时间选择器（DTP）+ 热键控件（v1.12.0）

    // v1.13.24：标题/副标题/气泡/提醒字体与对勾画笔集中创建（随 DPI 重建）
    CreateUiResources();
    g_fontGroup   = MakeFont(g_grpFontSize);   // v1.13.0 分组字体（大小可设置）
    g_fontGroupBold = MakeFont(g_grpFontSize, true);   // v0.25 未完成分组标题加粗
    g_fontGroupDone = MakeFont(g_doneGrpFontSize);     // v1.13.6 已完成分组字体
    g_fontDate    = MakeFont(g_dtFontSize);    // v0.26 日期时间字体（大小可设置）
    g_font        = MakeFont(g_taskFontSize);  // v1.13.0 任务字体（大小可设置）
    g_fontDone    = MakeFont(g_taskFontSize, false, true);
    g_brWhite   = CreateSolidBrush(COL_WHITE);
    // v0.27：窗口类背景刷随主题重建（ApplyTheme 里重新创建并 SetClassLongPtr）
    g_brBg      = CreateSolidBrush(COL_BG);

    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    // v1.13.24：大/小图标都按系统当前尺寸取（标题栏与任务栏各自命中最合适的
    // 一张），而不是把 32x32 直接塞给小图标位再缩小 —— 那会糊成一团
    wc.hIcon         = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON),
                                         GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"TicklyClass";
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // v1.13.0：自绘列表窗口类（替代 ListBox，像素级平滑滚动）
    // v0.15：补注册 CS_DBLCLKS —— v1.13.0 漏掉后双击不再产生
    // WM_LBUTTONDBLCLK，任务无法双击进入行内编辑
    WNDCLASSEXW lc {};
    lc.cbSize        = sizeof(lc);
    lc.style         = CS_DBLCLKS;
    lc.lpfnWndProc   = TodoListProc;
    lc.hInstance     = hInst;
    lc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    lc.hbrBackground = nullptr;
    lc.lpszClassName = L"TicklyList";
    RegisterClassExW(&lc);

    // v1.12.0：划词结果气泡窗口类
    WNDCLASSEXW tc {};
    tc.cbSize        = sizeof(tc);
    tc.lpfnWndProc   = ToastProc;
    tc.hInstance     = hInst;
    tc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    tc.hbrBackground = nullptr;
    tc.lpszClassName = L"TicklyToast";
    RegisterClassExW(&tc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT wr = { 0, 0, Scale(480), Scale(660) };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tickly",
                             style, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                             nullptr, nullptr, hInst, nullptr);

    RECT wa {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_hwnd, nullptr,
                 wa.left + ((wa.right - wa.left) - ww) / 2,
                 wa.top  + ((wa.bottom - wa.top) - wh) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    LoadTasks();
    ApplyTheme();                        // v0.27：按加载的主题模式应用色板/DWM
    ApplyFontSettings();                 // v1.13.0：按加载的字体设置重建字体
    RefreshList();
    UpdateButtons();
    ApplyHotkeys();                      // v1.12.0：注册全局快捷键（Alt+Q / Alt+1）

    UINT showCmd = (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE)
                       ? SW_SHOWNORMAL : (UINT)nCmdShow;
    if (g_wpSaved) {                     // 恢复上次的窗口大小/位置
        RECT wa {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const RECT& r = g_wpRect;
        bool ok = (r.right - r.left) >= Scale(220) &&
                  (r.bottom - r.top) >= Scale(320) &&
                  r.right > wa.left + Scale(100) && r.left < wa.right - Scale(100) &&
                  r.bottom > wa.top + Scale(30) && r.top < wa.bottom - Scale(30);
        if (ok)
            SetWindowPos(g_hwnd, nullptr, r.left, r.top,
                         r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
        if (g_wpMax) showCmd = SW_SHOWMAXIMIZED;
    }
    ShowWindow(g_hwnd, (int)showCmd);
    UpdateWindow(g_hwnd);
    CheckReminders();   // 启动时若有到期/逾期任务，立即提醒

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!g_rowEdit && IsDialogMessageW(g_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return (int)msg.wParam;
}
