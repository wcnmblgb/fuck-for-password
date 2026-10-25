# -*- coding: utf-8 -*-
# 作者：lanzongnb666
"""
自动点击穷举数字密码（4 位 0-9）
纯 ctypes 实现，不需要安装任何第三方库，只要有 Python 就能跑。

目标界面（手机锁屏样式的密码框）：
    数字键盘 1..9、0，底排三个按钮：←（退格）、清零、确定

每跑一组密码的完整动作：
    点 4 个数字键 -> 点“确定” -> 等判定 -> 查颜色监视点 -> 关失败提示 -> 点“清零” -> 下一组

颜色检测（判断密码对不对）
    密码正确时密码轮盘会自动关闭，也就是屏幕上某个点会“变色”。
    配好 WATCH_POINT（或在校准脚本里抓）之后，脚本每点完一次“确定”
    就读一次该点的颜色，和基准色差得超过 TOLERANCE 就判定为“变色”，
    把这一组记到 hits.txt 并在日志里打 [HIT] 标记 —— 但默认不停止，继续往下跑，
    你自己看着屏幕，看到轮盘真的关了再按 ESC 停。

用法
----
1) 先打开目标程序，把密码窗口摆好，之后不要再移动窗口；
2) 运行：  python 穷举密码.py
           启动时会问你“初始密码”，直接回车 = 从 0000 开始；
           python 穷举密码.py 1234       命令行直接给，跳过询问
3) 有 3 秒倒计时，切到目标窗口（鼠标不要乱动）；
4) 急停：按 ESC，或把鼠标甩到屏幕左上角。脚本每点完一组都会检查；
5) 日志：D:\\db\\brute_log.txt（含 [HIT] 标记）、D:\\db\\hits.txt（只记变色那几组）。

先核对坐标（强烈建议第一次这样做）
--------------------------------
    python 穷举密码.py --test
鼠标会依次停在每个数字键、确定、清零、监视点的坐标上，你肉眼确认。

坐标不准就重新抓：
    python 校准坐标.py
它会生成 D:\\db\\coords.json，本脚本会自动优先使用里面的坐标。

只在自己有权操作的设备/软件上使用。
"""

import ctypes
import json
import os
import sys
import time
from ctypes import wintypes

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
COORDS_FILE = os.path.join(BASE_DIR, 'coords.json')
LOG_FILE = os.path.join(BASE_DIR, 'brute_log.txt')
HITS_FILE = os.path.join(BASE_DIR, 'hits.txt')

# ============================ 配置区（按需修改） ============================

LENGTH = 4              # 密码位数

# 初始密码（起点）：
#   - 填了 START_CODE（例 '1234'）就从它开始，启动时不再问你
#   - 命令行给也行：python 穷举密码.py 1234（优先于 START_CODE）
#   - 都没给、且 ASK_START = True 时，启动时会问你一次，直接回车 = 从 0000 开始
START_CODE = ''
START_INDEX = 0         # 兜底起点（0000），一般不用改
ASK_START = True        # 启动时询问初始密码

# ---- 速度：想更快就继续调小这几个值，调到你机器上不出错为止 ----
CLICK_INTERVAL = 0.02   # 数字键之间的间隔（秒）
SUBMIT_WAIT = 0.35      # 点“确定”之后的等待，等程序判定这次对不对
FAIL_WAIT = 0.15        # 关掉失败提示之后的等待
CLEAR_WAIT = 0.03       # 点“清零”之后的等待：清零只是清输入框，不用犹豫，0 都行

# “确定”按钮坐标（界面底排右边的“确定”），例：(1600, 860)
# 留 None 表示用回车键提交
SUBMIT_POINT = None

# “清零”按钮坐标（界面底排中间的“清零”）：每轮跑完点它清空输入框
# 重要：不点清零的话，第二组开始就会在上一组残留的数字后面接着输，必错
CLEAR_POINT = None

# 清零也可以走键盘（有的程序按 Esc 就是清空/重输）：'enter' / 'esc' / None
CLEAR_KEY = None

# 失败弹窗的关闭按钮坐标，例：(1600, 700)；None 表示不点坐标
FAIL_CLOSE_POINT = None

# 失败弹窗用键盘关：'enter' / 'esc' / None。如果失败时程序没有弹窗，
# 把这里和 FAIL_CLOSE_POINT 都设成 None 即可（那清零就承担了全部复位工作）
FAIL_CLOSE_KEY = None

# 顺序固定为：点数字 -> 确定 -> 等 -> 查颜色 -> 关失败提示 -> 点清零 -> 下一组
# 如果“关失败提示”和“清零”其实是同一个按钮，只填 CLEAR_POINT 即可
# （脚本发现两个坐标相同会自动只点一次）

# ---- 颜色检测：判断密码是否正确（密码对了轮盘会关，那个点的颜色就变了）----
WATCH_POINT = None      # 监视点坐标，例：(1600, 700)；None = 关闭检测
BASE_COLOR = None       # 基准颜色 (r, g, b)；None = 启动时自动采样一次
TOLERANCE = 30          # 单通道差值超过它就算“变色”，误报多就调大
STOP_ON_HIT = False     # 变色后是否立刻停下；默认 False = 只标记，继续跑

# ---- 数字键坐标（放大镜抓的点；快捷键后面的数字 = 键位）----
DEFAULT_KEYS = {
    1: (1460, 609),
    2: (1550, 610),
    3: (1654, 611),
    4: (1456, 687),
    5: (1552, 680),
    6: (1659, 684),
    7: (1457, 762),
    8: (1555, 762),
    9: (1662, 758),
    0: (1556, 825),
}

# ============================ 底层鼠标键盘操作 ============================

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetCursorPos.restype = wintypes.BOOL
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.ReleaseDC.restype = ctypes.c_int
gdi32.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.GetPixel.restype = wintypes.DWORD

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
VK_ESCAPE = 0x1B
VK_RETURN = 0x0D
CLR_INVALID = 0xFFFFFFFF


class POINT(ctypes.Structure):
    _fields_ = [('x', wintypes.LONG), ('y', wintypes.LONG)]


def set_dpi_aware():
    """按物理像素工作，坐标才能和放大镜抓到的一致（高分屏缩放必需）"""
    try:
        ctypes.WinDLL('shcore').SetProcessDpiAwareness(2)
        return
    except Exception:
        pass
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass


def get_cursor_pos():
    p = POINT()
    user32.GetCursorPos(ctypes.byref(p))
    return (p.x, p.y)


def get_pixel(x, y):
    """读屏幕上一个点的颜色，返回 (r, g, b)；读不到返回 None"""
    hdc = user32.GetDC(None)
    if not hdc:
        return None
    try:
        c = gdi32.GetPixel(hdc, int(x), int(y))
    finally:
        user32.ReleaseDC(None, hdc)
    if c == CLR_INVALID:
        return None
    return (int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF))


def color_diff(a, b):
    """两个颜色的最大单通道差值"""
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def hexcolor(c):
    return '#%02X%02X%02X' % (c[0], c[1], c[2])


def click(x, y, hold=0.015):
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.006)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(hold)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def press_key(vk, hold=0.02):
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(hold)
    user32.keybd_event(vk, 0, 2, 0)   # KEYEVENTF_KEYUP


def esc_pressed():
    return bool(user32.GetAsyncKeyState(VK_ESCAPE) & 0x8000)


def failsafe_hit():
    """鼠标被甩到屏幕左上角 = 立刻停（保险绳）"""
    x, y = get_cursor_pos()
    return x <= 3 and y <= 3


def stop_requested():
    return esc_pressed() or failsafe_hit()


# ============================ 配置装载 ============================

def load_config():
    keys = dict(DEFAULT_KEYS)
    cfg = {
        'length': LENGTH,
        'start_code': START_CODE,
        'start': START_INDEX,
        'ask_start': ASK_START,
        'click_interval': CLICK_INTERVAL,
        'submit_wait': SUBMIT_WAIT,
        'fail_wait': FAIL_WAIT,
        'clear_wait': CLEAR_WAIT,
        'submit': (tuple(SUBMIT_POINT) if SUBMIT_POINT else None),
        'clear_point': (tuple(CLEAR_POINT) if CLEAR_POINT else None),
        'clear_key': CLEAR_KEY,
        'fail_close_point': (tuple(FAIL_CLOSE_POINT) if FAIL_CLOSE_POINT else None),
        'fail_close_key': FAIL_CLOSE_KEY,
        'watch_point': (tuple(WATCH_POINT) if WATCH_POINT else None),
        'base_color': (tuple(BASE_COLOR) if BASE_COLOR else None),
        'tolerance': TOLERANCE,
        'stop_on_hit': STOP_ON_HIT,
    }

    if os.path.exists(COORDS_FILE):
        try:
            with open(COORDS_FILE, 'r', encoding='utf-8') as f:
                data = json.load(f)
            for k, v in (data.get('keys') or {}).items():
                keys[int(k)] = (int(v[0]), int(v[1]))
            for name in ('submit', 'clear_point', 'fail_close_point', 'watch_point'):
                if data.get(name):
                    cfg[name] = (int(data[name][0]), int(data[name][1]))
            for name in ('clear_key', 'fail_close_key'):
                if data.get(name) is not None:
                    cfg[name] = data[name]
            if data.get('base_color'):
                cfg['base_color'] = tuple(int(v) for v in data['base_color'])
            if data.get('tolerance') is not None:
                cfg['tolerance'] = int(data['tolerance'])
            if data.get('stop_on_hit') is not None:
                cfg['stop_on_hit'] = bool(data['stop_on_hit'])
            print('[i] 已加载坐标文件：%s' % COORDS_FILE)
        except Exception as e:
            print('[!] 读取 coords.json 失败，改用脚本内置配置：%s' % e)

    return keys, cfg


def fmt_code(idx, length):
    return str(idx).zfill(length)


def ask_start(length, default):
    """启动时问一次初始密码；直接回车 = 用默认值"""
    while True:
        try:
            ans = input('请输入初始密码（%d 位数字，直接回车 = %s）：'
                        % (length, default)).strip()
        except EOFError:
            return int(default)
        if not ans:
            return int(default)
        if ans.isdigit() and len(ans) <= length:
            return int(ans)
        print('  [!] 请输入不超过 %d 位的纯数字' % length)


# ============================ 动作 ============================

def submit(cfg):
    if cfg['submit']:
        click(*cfg['submit'])
    else:
        press_key(VK_RETURN)


def close_fail(cfg):
    if cfg['fail_close_point']:
        click(*cfg['fail_close_point'])
        time.sleep(cfg['fail_wait'])
        return
    key = cfg['fail_close_key']
    if key == 'enter':
        press_key(VK_RETURN)
        time.sleep(cfg['fail_wait'])
    elif key == 'esc':
        press_key(VK_ESCAPE)
        time.sleep(cfg['fail_wait'])


def clear_input(cfg, already_clicked=None):
    """点“清零”，把输入框里上一组残留的数字清掉"""
    pt = cfg['clear_point']
    if pt:
        if already_clicked and tuple(pt) == tuple(already_clicked):
            return          # 和上一步点的按钮是同一个，不重复点
        click(*pt)
        time.sleep(cfg['clear_wait'])
        return
    key = cfg['clear_key']
    if key == 'enter':
        press_key(VK_RETURN)
        time.sleep(cfg['clear_wait'])
    elif key == 'esc':
        press_key(VK_ESCAPE)
        time.sleep(cfg['clear_wait'])


def test_mode(keys, cfg):
    print('=== 坐标自检：鼠标会依次停在每个键/按钮上，眼睛看着屏幕确认 ===')
    print('    键盘布局 1-2-3 / 4-5-6 / 7-8-9 三列，0 在下面一行')
    time.sleep(1.0)
    for d in [1, 2, 3, 4, 5, 6, 7, 8, 9, 0]:
        x, y = keys[d]
        user32.SetCursorPos(int(x), int(y))
        print('    数字 %d -> 屏幕坐标 (%d, %d)' % (d, x, y))
        time.sleep(0.9)
    for name, pt in (('确定', cfg['submit']),
                     ('清零', cfg['clear_point']),
                     ('失败弹窗关闭', cfg['fail_close_point']),
                     ('颜色监视点', cfg['watch_point'])):
        if pt:
            user32.SetCursorPos(int(pt[0]), int(pt[1]))
            extra = ''
            if name == '颜色监视点':
                c = get_pixel(pt[0], pt[1])
                if c:
                    extra = '  当前颜色 %s' % hexcolor(c)
            print('    %s -> 屏幕坐标 (%d, %d)%s' % (name, pt[0], pt[1], extra))
            time.sleep(0.9)
        else:
            print('    %s -> 未配置' % name)
    print('=== 自检结束。哪个位置不对，就运行 校准坐标.py 重新抓 ===')


def run(keys, cfg):
    length = cfg['length']
    total = 10 ** length

    start = cfg['start']
    given = str(cfg.get('start_code') or '').strip()
    if given:
        try:
            start = int(given)
        except ValueError:
            print('[!] 初始密码 %r 不是纯数字，忽略' % given)
    elif cfg.get('ask_start', True):
        start = ask_start(length, fmt_code(start, length))
    start = max(0, min(start, total - 1))

    print('==== 运行配置 ====')
    print('  密码位数   : %d（%s ~ %s，共 %d 组）'
          % (length, fmt_code(0, length), fmt_code(total - 1, length), total))
    print('  初始密码   : %s' % fmt_code(start, length))
    print('  数字键坐标 : ' + '  '.join('%d->(%d,%d)' % (d, keys[d][0], keys[d][1])
                                        for d in [1, 2, 3, 4, 5, 6, 7, 8, 9, 0]))
    print('  提交方式   : %s' % (('点击 (%d,%d)' % cfg['submit']) if cfg['submit'] else '回车键'))
    print('  关失败提示 : %s' % ((('点击 (%d,%d)' % cfg['fail_close_point']) if cfg['fail_close_point']
                                 else ('按键 %s' % cfg['fail_close_key'])
                                 if cfg['fail_close_key'] else '不处理')))
    print('  清零方式   : %s' % ((('点击 (%d,%d)' % cfg['clear_point']) if cfg['clear_point']
                                 else ('按键 %s' % cfg['clear_key'])
                                 if cfg['clear_key'] else '！未配置（第二组起会叠加残留数字）')))
    print('  每轮耗时   : 约 %.2f 秒' % (length * (cfg['click_interval'] + 0.04)
                                          + cfg['submit_wait'] + cfg['fail_wait'] + cfg['clear_wait']))
    print('  颜色检测   : %s' % (('监视点 (%d,%d) 容差 %d，变色后%s'
                                 % (cfg['watch_point'][0], cfg['watch_point'][1], cfg['tolerance'],
                                    '停止' if cfg['stop_on_hit'] else '只标记不停'))
                                 if cfg['watch_point'] else '未配置（不做判定）'))
    print('  日志文件   : %s' % LOG_FILE)
    print('  变色记录   : %s' % HITS_FILE)
    print('  急停方式   : ESC 键 或 鼠标甩到屏幕左上角')
    print()

    if not cfg['clear_point'] and not cfg['clear_key']:
        print('[!] 没配清零：如果目标程序不会自动清空输入框，第二组开始就是错的。')
        print('[!] 先运行  python 校准坐标.py  抓“清零”和“确定”的坐标。')
        print()
    if not cfg['watch_point']:
        print('[!] 没配颜色监视点：脚本无法判断密码对不对，只能靠你自己盯屏幕。')
        print()

    log = open(LOG_FILE, 'a', encoding='utf-8')
    hits = open(HITS_FILE, 'a', encoding='utf-8')
    log.write('\n===== 新一轮 %s  起始=%s =====\n'
              % (time.strftime('%Y-%m-%d %H:%M:%S'), fmt_code(start, length)))
    log.flush()

    print('3 秒后开始，请切到目标窗口……')
    for s in (3, 2, 1):
        print('  %d' % s)
        time.sleep(1.0)

    # 基准颜色：此刻密码轮盘还开着，趁现在采样
    base = cfg['base_color']
    if cfg['watch_point'] and not base:
        base = get_pixel(*cfg['watch_point'])
        if base:
            print('[i] 自动采样基准颜色：%s（监视点 (%d,%d)）'
                  % (hexcolor(base), cfg['watch_point'][0], cfg['watch_point'][1]))
        else:
            print('[!] 基准颜色采样失败，颜色检测关闭')
    elif base and cfg['watch_point']:
        print('[i] 使用配置的基准颜色：%s' % hexcolor(base))

    done = start
    hit_count = 0
    t0 = time.time()
    try:
        for idx in range(start, total):
            if stop_requested():
                break
            code = fmt_code(idx, length)
            for ch in code:
                if stop_requested():
                    break
                click(*keys[int(ch)])
                time.sleep(cfg['click_interval'])
            if stop_requested():
                break

            submit(cfg)
            time.sleep(cfg['submit_wait'])

            # 点完“确定”之后看一眼监视点：变色 = 密码可能对了
            changed = False
            cur = None
            if cfg['watch_point'] and base:
                cur = get_pixel(*cfg['watch_point'])
                if cur:
                    diff = color_diff(cur, base)
                    if diff > cfg['tolerance']:
                        changed = True
                        hit_count += 1
                        stamp = time.strftime('%Y-%m-%d %H:%M:%S')
                        hits.write('%s  组合=%s  颜色=%s  基准=%s  差值=%d\n'
                                   % (stamp, code, hexcolor(cur), hexcolor(base), diff))
                        hits.flush()
                        print('\n  *** [HIT] 第 %d 次变色：组合 %s  颜色 %s（基准 %s，差值 %d）***'
                              % (hit_count, code, hexcolor(cur), hexcolor(base), diff))

            close_fail(cfg)
            clear_input(cfg, cfg['fail_close_point'])

            done = idx
            log.write('%s  %s  %s elapsed=%.0fs\n'
                      % (time.strftime('%H:%M:%S'), code,
                         '[HIT 变色]' if changed else '         ',
                         time.time() - t0))
            log.flush()
            print('\r  正在尝试第 %d/%d 组：%s  已变色 %d 次   '
                  % (idx + 1, total, code, hit_count), end='', flush=True)

            if changed and cfg['stop_on_hit']:
                print('\n  STOP_ON_HIT = True，变色后按配置停止')
                break
    except KeyboardInterrupt:
        print('\n[!] 收到 Ctrl+C')
    finally:
        log.write('----- 停止于 %s（已尝试到 %s，变色 %d 次）-----\n'
                  % (time.strftime('%Y-%m-%d %H:%M:%S'), fmt_code(done, length), hit_count))
        log.close()
        hits.close()

    print()
    print('==== 结束 ====')
    print('  最后尝试的组合：%s' % fmt_code(done, length))
    print('  变色（可能是对的）次数：%d' % hit_count)
    print('  日志：%s' % LOG_FILE)
    if hit_count:
        print('  变色记录：%s（挑时间最早那条，通常就是密码）' % HITS_FILE)
    if done >= total - 1 and not stop_requested():
        print('  已经把 %d 组全部试完了，如果还不对，说明密码位数/按键坐标/清零有问题'
              % total)
    elif not hit_count:
        print('  如果你是在成功那一刻按的 ESC，那 %s 就是密码（日志最后一行同名）'
              % fmt_code(done, length))


def main():
    set_dpi_aware()
    keys, cfg = load_config()

    # 命令行的裸参数当作初始密码：python 穷举密码.py 1234
    plain = [a for a in sys.argv[1:] if not a.startswith('--')]
    if plain:
        cfg['start_code'] = plain[0]

    if '--test' in sys.argv:
        test_mode(keys, cfg)
        return
    run(keys, cfg)


if __name__ == '__main__':
    main()
