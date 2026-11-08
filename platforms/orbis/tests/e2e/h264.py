#!/usr/bin/env python3
"""Writes orbis_process_test's H.264 stream (videodec2.c's input) as C:
12 access units of a 1920x1088 High-profile stream (cropped to 1080),
CAVLC, every picture intra. Each picture is flat (Intra16x16 DC with no
residual: 128 in luma and chroma) but for its last macroblock row, raw
(I_PCM) samples of Pattern(display index). Pictures are decoded in
another order than shown (DECODE_ORDER: display indices, at most 2
ahead, as num_reorder_frames says); the VUI has timing (1/120) and the
bitstream restrictions, max_dec_frame_buffering = max_num_ref_frames = 4.

usage: h264.py OUT.c
"""
import sys

W_MBS, H_MBS = 120, 68
DECODE_ORDER = [0, 3, 1, 2, 6, 4, 5, 9, 7, 8, 11, 10]


def pattern(d, plane, x, y):
    """A sample of the last macroblock row of display picture d (x, y
    within the plane: luma 0, Cb 1, Cr 2). Never below 16, so the raw
    samples need no emulation prevention of their own."""
    if plane == 0:
        return 32 + 8 * d + (x + 3 * y) % 16
    if plane == 1:
        return 64 + 4 * d + x % 8
    return 160 - 4 * d + y % 4


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def align_zero(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def trailing(self):
        self.bits.append(1)
        self.align_zero()

    def raw(self, data):
        assert len(self.bits) % 8 == 0
        for b in data:
            self.u(8, b)

    def bytes(self):
        assert len(self.bits) % 8 == 0
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def nal(ref_idc, kind, rbsp):
    out = bytearray(b"\x00\x00\x00\x01")
    out.append(ref_idc << 5 | kind)
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def sps():
    b = Bits()
    b.u(8, 100)  # profile_idc: High
    b.u(8, 0)
    b.u(8, 42)  # level_idc
    b.ue(0)  # seq_parameter_set_id
    b.ue(1)  # chroma_format_idc: 4:2:0
    b.ue(0)
    b.ue(0)  # bit depths 8
    b.u(1, 0)
    b.u(1, 0)  # no transform bypass, no scaling matrices
    b.ue(0)  # log2_max_frame_num - 4
    b.ue(0)  # pic_order_cnt_type
    b.ue(2)  # log2_max_pic_order_cnt_lsb - 4
    b.ue(4)  # max_num_ref_frames
    b.u(1, 0)
    b.ue(W_MBS - 1)
    b.ue(H_MBS - 1)
    b.u(1, 1)  # frame_mbs_only_flag
    b.u(1, 1)  # direct_8x8_inference_flag
    b.u(1, 1)  # frame_cropping_flag: bottom 4 (8 rows)
    for v in (0, 0, 0, 4):
        b.ue(v)
    b.u(1, 1)  # vui_parameters_present_flag
    b.u(1, 0)
    b.u(1, 0)
    b.u(1, 0)
    b.u(1, 0)  # aspect ratio, overscan, video signal, chroma location
    b.u(1, 1)  # timing_info_present_flag
    b.u(32, 1)
    b.u(32, 120)
    b.u(1, 0)  # fixed_frame_rate_flag
    b.u(1, 0)
    b.u(1, 0)
    b.u(1, 0)  # no HRD, no pic_struct
    b.u(1, 1)  # bitstream_restriction_flag
    b.u(1, 1)  # motion_vectors_over_pic_boundaries_flag
    b.ue(0)
    b.ue(0)
    b.ue(11)
    b.ue(11)
    b.ue(2)  # max_num_reorder_frames
    b.ue(4)  # max_dec_frame_buffering
    b.trailing()
    return nal(3, 7, b.bytes())


def pps():
    b = Bits()
    b.ue(0)
    b.ue(0)
    b.u(1, 0)  # CAVLC
    b.u(1, 0)
    b.ue(0)  # one slice group
    b.ue(0)
    b.ue(0)
    b.u(1, 0)
    b.u(2, 0)
    b.se(0)
    b.se(0)
    b.se(0)
    b.u(1, 1)  # deblocking_filter_control_present_flag
    b.u(1, 0)
    b.u(1, 0)
    b.trailing()
    return nal(3, 8, b.bytes())


def picture(decode_index, d):
    idr = decode_index == 0
    b = Bits()
    b.ue(0)  # first_mb_in_slice
    b.ue(7)  # slice_type: I (all)
    b.ue(0)
    b.u(4, decode_index % 16)  # frame_num (every picture a reference)
    if idr:
        b.ue(0)  # idr_pic_id
    b.u(6, 2 * d)  # pic_order_cnt_lsb: the display order
    if idr:
        b.u(1, 0)
        b.u(1, 0)
    else:
        b.u(1, 0)  # sliding window
    b.se(0)  # slice_qp_delta
    b.ue(1)  # deblocking off
    for my in range(H_MBS):
        for mx in range(W_MBS):
            if my < H_MBS - 1:
                b.ue(3)  # I_16x16_2_0_0: DC, no residual
                b.ue(0)  # intra_chroma_pred_mode: DC
                b.se(0)  # mb_qp_delta
                b.u(1, 1)  # Intra16x16DCLevel: no coefficients
                continue
            b.ue(25)  # I_PCM
            b.align_zero()
            b.raw(pattern(d, 0, 16 * mx + x, 16 * my + y) for y in range(16) for x in range(16))
            for plane in (1, 2):
                b.raw(pattern(d, plane, 8 * mx + x, 8 * my + y) for y in range(8) for x in range(8))
    b.trailing()
    return nal(3 if idr else 1, 5 if idr else 1, b.bytes())


def main():
    aus = []
    for i, d in enumerate(DECODE_ORDER):
        au = picture(i, d)
        aus.append(sps() + pps() + au if i == 0 else au)
    with open(sys.argv[1], "w") as f:
        f.write("// Generated by h264.py: the stream videodec2.c decodes.\n")
        for i, au in enumerate(aus):
            f.write("const unsigned char vd_au%d[%d] = {%s};\n" % (i, len(au), ",".join(map(str, au))))
        f.write("const unsigned char* const vd_aus[%d] = {%s};\n" %
                (len(aus), ",".join("vd_au%d" % i for i in range(len(aus)))))
        f.write("const unsigned long vd_au_sizes[%d] = {%s};\n" %
                (len(aus), ",".join(str(len(a)) for a in aus)))
        f.write("const int vd_display[%d] = {%s};\n" % (len(aus), ",".join(map(str, DECODE_ORDER))))


main()
