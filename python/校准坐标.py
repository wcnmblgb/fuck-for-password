# -*- coding: utf-8 -*-
# 作者：lanzongnb666
"""
坐标校准脚本：把鼠标依次移到每个按键/按钮上，自动记录坐标，存成 D:\db\coords.json
主脚本 穷举密码.py 会自动优先读取这个文件。

用法
----
    python 校准坐标.py

每一步都会提示“把鼠标移到【XX】上”，给你 3 秒（可改 WAIT_SECONDS），
倒计时结束后自动读取当前鼠标位置。中途想退出按 Ctrl+C。

采集顺序
--------
数字 1..9、0  ->  确定  ->  清零  ->  颜色监视点  ->  失败弹窗关闭按钮（可跳过）

颜色监视点是用来判断密码对不对的：密码正确时密码轮盘会自动关闭，
你挑一个“轮盘关掉后颜色会明显不一样”的位置（比如轮盘上的深色空白区），
脚本会同时记下它当前的颜色当作基准色。
不想抓的直接输 n 跳过。
"""

import ctypes
import json
import os
import sys
import time
from ctypes import wintypes

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
COORDS_FILE = os.path.join(BASE_DIR, 'coords.json')

WAIT_SECONDS = 3   # 每一步留给你移动鼠标的时间

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)

user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
gdi32.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.GetPixel.restype = wintypes.DWORD

CLR_INVALID = 0xFFFFFFFF


class POINT(ctypes.Structure):
    _fields_ = [('x', wintypes.LONG), ('y', wintypes.LONG)]


def set_dpi_aware():
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
    return (int(p.x), int(p.y))


def get_pixel(x, y):
    """读屏幕上某点的颜色，返回 (r, g, b)；读不到返回 None"""
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


def hexcolor(c):
    return '#%02X%02X%02X' % (c[0], c[1], c[2])


def wait_and_capture(what, seconds=WAIT_SECONDS):
    print('\n请在 %d 秒内把鼠标移到【%s】上，然后不要动……' % (seconds, what))
    for s in range(seconds, 0, -1):
        print('  %d' % s, end='', flush=True)
        time.sleep(1.0)
    x, y = get_cursor_pos()
    print('\n  -> 记录：%s = (%d, %d)' % (what, x, y))
    return (x, y)


def ask_and_capture(what, seconds=WAIT_SECONDS):
    ans = input('要抓【%s】吗？直接回车 = 是，输入 n = 跳过：' % what).strip().lower()
    if ans == 'n':
        return None
    return wait_and_capture(what, seconds)


def main():
    set_dpi_aware()
    print('=== 坐标校准（放大镜抓点的替代方案）===')
    print('先打开目标程序、让密码轮盘显示出来，再开始；校准期间别移动目标窗口。')

    keys = {}
    for d in [1, 2, 3, 4, 5, 6, 7, 8, 9, 0]:
        keys[str(d)] = wait_and_capture('数字 %d 键' % d)

    submit = ask_and_capture('确定按钮（底排右边那个）')
    clear_point = ask_and_capture('清零按钮（底排中间那个）')

    watch = ask_and_capture('颜色监视点（密码正确、轮盘关掉后颜色会明显变化的位置）')
    base_color = None
    if watch:
        base_color = get_pixel(*watch)
        if base_color:
            print('  -> 该点当前颜色（基准色）：%s' % hexcolor(base_color))
        else:
            print('  [!] 颜色读取失败，请检查监视点是否在屏幕范围内')

    fail_close = ask_and_capture('失败弹窗的关闭按钮（没有弹窗就输 n 跳过）')

    data = {
        'keys': keys,
        'submit': list(submit) if submit else None,
        'clear_point': list(clear_point) if clear_point else None,
        'watch_point': list(watch) if watch else None,
        'base_color': list(base_color) if base_color else None,
        'tolerance': 30,
        'stop_on_hit': False,
        'fail_close_point': list(fail_close) if fail_close else None,
        'fail_close_key': None,
    }
    with open(COORDS_FILE, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print('\n=== 已保存：%s ===' % COORDS_FILE)
    print(json.dumps(data, ensure_ascii=False, indent=2))
    print('\n下一步：python 穷举密码.py --test  先自检，再正式跑  python 穷举密码.py')
    print('提示：base_color 是“轮盘还开着”时的颜色。如果误报多，把 coords.json 里的')
    print('      tolerance 调大；如果正确时颜色变化很小，把它调小。')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\n[!] 已取消，未写入文件')
        sys.exit(1)
