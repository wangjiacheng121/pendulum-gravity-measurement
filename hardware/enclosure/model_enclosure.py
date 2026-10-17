# -*- coding: utf-8 -*-
"""
单摆测g 项目外壳 —— 嘉立创 X树脂 免费打样版 (两分体方案A, 搭接互锁)
坐标系: X 0=前->172=后, Y 0=左->右, Z 0=底->顶, 壁厚3
所有输出仅在 E 盘, 不占用 C 盘。

用法:
  python model_enclosure.py sweep
  python model_enclosure.py           # 按 FINAL 导出 STL
"""
import os, sys
import numpy as np
import trimesh
from trimesh.creation import box as mkbox, cylinder, extrude_polygon
from trimesh.boolean import union, difference
from shapely.geometry import box as sbox
from trimesh.transformations import rotation_matrix

HERE = os.path.dirname(os.path.abspath(__file__))
STL_DIR = os.path.join(HERE, "stl")
os.makedirs(STL_DIR, exist_ok=True)

# ---------------- 固定参数(全部 mm) ----------------
T = 3.0                       # 壁厚
L = 172.0                     # 外长(不变)
SPLIT = 86.0                  # 两分体理论切缝(X)
OL = 4.0                      # 搭接互锁重叠宽度(左前/后墙伸出, 右前/后墙凹进)

# 顶板屏幕口: 36x35 R2, 孔中心距左32、距前33
SCR_W, SCR_H, SCR_R = 36.0, 35.0, 2.0
SCR_CX, SCR_CY = 3.0 + 32.0, 3.0 + 33.0
# 按键杜邦线连孔: 一个圆角长槽(连接原2孔), 位于屏幕前方
BTN_XC, BTN_Y = SCR_CX, 10.0
BTN_SLOT_LEN, BTN_D = 8.0, 3.5          # 长槽: X长8, Y宽3.5
# 右板 Type-C 口: 横置矩形 15(Y) x 10(Z) R2, 长边水平; 底端距容器内底10mm(内底Z=3 -> 底Z=13)
TC_W, TC_H, TC_R = 15.0, 10.0, 2.0
TC_Y0, TC_Z0 = 24.5, 13.0               # 孔近边 Y=24.5(中心Y=32=右板居中), Z=13(底,距内底1cm)
TC_XC = 170.5                           # 右板厚向中心(X169..172)
# 光电门 3 线连孔: 一个圆角长槽(连接原3孔), 位于前板(左半), 近底部, 轴沿Y
WIRE_XC, WIRE_Z, WIRE_D = SCR_CX, 12.0, 3.5
WIRE_SLOT_LEN = 21.5                     # 长槽: X长21.5(覆盖26/35/44三孔)
CUT = 10.0

# ---------------- 基础工具 ----------------
def box(x0, x1, y0, y1, z0, z1):
    m = mkbox(extents=[x1-x0, y1-y0, z1-z0])
    m.apply_translation([(x0+x1)/2, (y0+y1)/2, (z0+z1)/2])
    return m

def rrect(w, h, depth, r):
    p = sbox(-w/2+r, -h/2+r, w/2-r, h/2-r).buffer(r, quad_segs=32)
    return extrude_polygon(p, height=depth)

def place(mesh, cx, cy, cz):
    b = mesh.bounds
    c = (b[0] + b[1]) / 2.0
    mesh.apply_translation([cx - c[0], cy - c[1], cz - c[2]])
    return mesh

def vol_cm3(m):
    return m.volume / 1000.0

def bbox(m):
    b = m.bounds
    return [round(float(b[1][i] - b[0][i]), 2) for i in range(3)]

# ---------------- 开孔刀具(随 H 参数化) ----------------
def make_cutters(H):
    CUT_Z = H - T/2.0                 # 顶板厚向中心(随高度参数化)
    scr = place(rrect(SCR_W, SCR_H, CUT, SCR_R), SCR_CX, SCR_CY, CUT_Z)
    btn = place(rrect(BTN_SLOT_LEN, BTN_D, CUT, BTN_D/2.0), BTN_XC, BTN_Y, CUT_Z)  # 按键连孔
    top = union([scr, btn], engine="manifold")

    tc = rrect(TC_W, TC_H, CUT, TC_R)
    tc.apply_transform(rotation_matrix(np.pi/2, [0, 0, 1]))
    tc.apply_transform(rotation_matrix(np.pi/2, [0, 1, 0]))
    tc = place(tc, TC_XC, TC_Y0 + TC_W/2.0, TC_Z0 + TC_H/2.0)   # 底Z=13

    wr = rrect(WIRE_SLOT_LEN, WIRE_D, CUT, WIRE_D/2.0)          # 光电门连孔(轴沿Y)
    wr.apply_transform(rotation_matrix(np.pi/2, [1, 0, 0]))
    wr = place(wr, WIRE_XC, 1.5, WIRE_Z)
    return top, tc, wr

def build_trays(W, H):
    # 左半: 前/后墙伸出 OL 作雄榫; 底板/顶板/左墙止于 SPLIT
    lp = [box(0, SPLIT+OL, 0, T, 0, H), box(0, SPLIT+OL, W-T, W, 0, H),
          box(0, T, T, W-T, 0, H), box(T, SPLIT, T, W-T, 0, T),
          box(T, SPLIT, T, W-T, H-T, H)]
    # 右半: 前/后墙从 SPLIT+OL 起(凹进 OL 作雌槽); 底板/顶板/右墙从 SPLIT 起
    rp = [box(SPLIT+OL, L, 0, T, 0, H), box(SPLIT+OL, L, W-T, W, 0, H),
          box(L-T, L, T, W-T, 0, H), box(SPLIT, L-T, T, W-T, 0, T),
          box(SPLIT, L-T, T, W-T, H-T, H)]
    Lm = union(lp, engine="manifold")
    Rm = union(rp, engine="manifold")
    top, tc, wr = make_cutters(H)
    Lm = difference([Lm, top], engine="manifold")    # 屏幕口+按键连孔
    Rm = difference([Rm, tc], engine="manifold")     # Type-C口
    Lm = difference([Lm, wr], engine="manifold")     # 光电门连孔
    return Lm, Rm

def build_assembled(W, H):
    """左右两半 union 成一个水密整体(仅预览校验用, 不能打样: 172mm/133cm3 超免费券)"""
    Lm, Rm = build_trays(W, H)
    A = union([Lm, Rm], engine="manifold")
    return A

# ---------------- 主流程 ----------------
if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "sweep":
        print("外宽Wo 外高Ho | 内尺寸(长x宽x高) | 左壳cm3 右壳cm3 | bbox | <=70?")
        for Wo in [64.0]:
            for Ho in [56.0, 58.0, 59.0, 60.0]:
                Lm, Rm = build_trays(Wo, Ho)
                iw, ih = Wo - 2*T, Ho - 2*T
                ok = "OK" if (vol_cm3(Lm) <= 70 and vol_cm3(Rm) <= 70) else "超"
                print(f"Wo={Wo:.0f} Ho={Ho:.0f} | 内={L-2*T:.0f}x{iw:.0f}x{ih:.0f} | "
                      f"L={vol_cm3(Lm):.2f} R={vol_cm3(Rm):.2f} | bbox={bbox(Lm)} | {ok}")
    else:
        Wo, Ho = 64.0, 58.0          # 内宽58 内高52
        Lm, Rm = build_trays(Wo, Ho)
        for name, m in [("shell_left_tray", Lm), ("shell_right_tray", Rm)]:
            p = os.path.join(STL_DIR, name + ".stl")
            m.export(p)
            print(f"{name}: bbox={bbox(m)} 体积={vol_cm3(m):.2f}cm3 水密={m.is_watertight}")
        # ---- 拼接完成后的整体模型(预览校验用, 不可打样) ----
        A = build_assembled(Wo, Ho)
        pa = os.path.join(STL_DIR, "shell_assembled.stl")
        A.export(pa)
        print(f"shell_assembled: bbox={bbox(A)} 体积={vol_cm3(A):.2f}cm3 水密={A.is_watertight}")
        print("内尺寸 =", [L-2*T, Wo-2*T, Ho-2*T], "搭接OL=", OL)
        print("STL ->", STL_DIR)
        # 整包(含整体装配模型)
        import zipfile
        zp = os.path.join(HERE, "外壳STL_含整体装配模型.zip")
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in ["shell_left_tray.stl", "shell_right_tray.stl", "shell_assembled.stl"]:
                zf.write(os.path.join(STL_DIR, f), "外壳STL/" + f)
        print("整包 ->", zp, round(os.path.getsize(zp)/1024, 1), "KB")
