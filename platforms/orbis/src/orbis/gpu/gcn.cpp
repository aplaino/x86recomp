#include "orbis/gpu/gcn.h"

#include <cstdio>
#include <map>
#include <utility>

namespace recomp::orbis::gpu::gcn {
namespace {

using Names = std::map<uint32_t, const char*>;

const Names kSop2 = {
    {0, "s_add_u32"}, {1, "s_sub_u32"}, {2, "s_add_i32"}, {3, "s_sub_i32"},
    {4, "s_addc_u32"}, {5, "s_subb_u32"}, {6, "s_min_i32"}, {7, "s_min_u32"},
    {8, "s_max_i32"}, {9, "s_max_u32"}, {10, "s_cselect_b32"}, {11, "s_cselect_b64"},
    {14, "s_and_b32"}, {15, "s_and_b64"}, {16, "s_or_b32"}, {17, "s_or_b64"},
    {18, "s_xor_b32"}, {19, "s_xor_b64"}, {20, "s_andn2_b32"}, {21, "s_andn2_b64"},
    {22, "s_orn2_b32"}, {23, "s_orn2_b64"}, {24, "s_nand_b32"}, {25, "s_nand_b64"},
    {26, "s_nor_b32"}, {27, "s_nor_b64"}, {28, "s_xnor_b32"}, {29, "s_xnor_b64"},
    {30, "s_lshl_b32"}, {31, "s_lshl_b64"}, {32, "s_lshr_b32"}, {33, "s_lshr_b64"},
    {34, "s_ashr_i32"}, {35, "s_ashr_i64"}, {36, "s_bfm_b32"}, {37, "s_bfm_b64"},
    {38, "s_mul_i32"}, {39, "s_bfe_u32"}, {40, "s_bfe_i32"}, {41, "s_bfe_u64"},
    {42, "s_bfe_i64"}, {43, "s_cbranch_g_fork"}, {44, "s_absdiff_i32"},
};
const Names kSopk = {
    {0, "s_movk_i32"}, {2, "s_cmovk_i32"}, {3, "s_cmpk_eq_i32"}, {4, "s_cmpk_lg_i32"},
    {5, "s_cmpk_gt_i32"}, {6, "s_cmpk_ge_i32"}, {7, "s_cmpk_lt_i32"}, {8, "s_cmpk_le_i32"},
    {9, "s_cmpk_eq_u32"}, {10, "s_cmpk_lg_u32"}, {11, "s_cmpk_gt_u32"}, {12, "s_cmpk_ge_u32"},
    {13, "s_cmpk_lt_u32"}, {14, "s_cmpk_le_u32"}, {15, "s_addk_i32"}, {16, "s_mulk_i32"},
    {17, "s_cbranch_i_fork"}, {18, "s_getreg_b32"}, {19, "s_setreg_b32"},
    {21, "s_setreg_imm32_b32"},
};
const Names kSop1 = {
    {3, "s_mov_b32"}, {4, "s_mov_b64"}, {5, "s_cmov_b32"}, {6, "s_cmov_b64"},
    {7, "s_not_b32"}, {8, "s_not_b64"}, {9, "s_wqm_b32"}, {10, "s_wqm_b64"},
    {11, "s_brev_b32"}, {12, "s_brev_b64"}, {13, "s_bcnt0_i32_b32"}, {14, "s_bcnt0_i32_b64"},
    {15, "s_bcnt1_i32_b32"}, {16, "s_bcnt1_i32_b64"}, {17, "s_ff0_i32_b32"},
    {18, "s_ff0_i32_b64"}, {19, "s_ff1_i32_b32"}, {20, "s_ff1_i32_b64"},
    {21, "s_flbit_i32_b32"}, {22, "s_flbit_i32_b64"}, {23, "s_flbit_i32"},
    {24, "s_flbit_i32_i64"}, {25, "s_sext_i32_i8"}, {26, "s_sext_i32_i16"},
    {27, "s_bitset0_b32"}, {28, "s_bitset0_b64"}, {29, "s_bitset1_b32"}, {30, "s_bitset1_b64"},
    {31, "s_getpc_b64"}, {32, "s_setpc_b64"}, {33, "s_swappc_b64"}, {34, "s_rfe_b64"},
    {36, "s_and_saveexec_b64"}, {37, "s_or_saveexec_b64"}, {38, "s_xor_saveexec_b64"},
    {39, "s_andn2_saveexec_b64"}, {40, "s_orn2_saveexec_b64"}, {41, "s_nand_saveexec_b64"},
    {42, "s_nor_saveexec_b64"}, {43, "s_xnor_saveexec_b64"}, {44, "s_quadmask_b32"},
    {45, "s_quadmask_b64"}, {46, "s_movrels_b32"}, {47, "s_movrels_b64"},
    {48, "s_movreld_b32"}, {49, "s_movreld_b64"}, {50, "s_cbranch_join"}, {52, "s_abs_i32"},
    {53, "s_mov_fed_b32"},
};
const Names kSopc = {
    {0, "s_cmp_eq_i32"}, {1, "s_cmp_lg_i32"}, {2, "s_cmp_gt_i32"}, {3, "s_cmp_ge_i32"},
    {4, "s_cmp_lt_i32"}, {5, "s_cmp_le_i32"}, {6, "s_cmp_eq_u32"}, {7, "s_cmp_lg_u32"},
    {8, "s_cmp_gt_u32"}, {9, "s_cmp_ge_u32"}, {10, "s_cmp_lt_u32"}, {11, "s_cmp_le_u32"},
    {12, "s_bitcmp0_b32"}, {13, "s_bitcmp1_b32"}, {14, "s_bitcmp0_b64"}, {15, "s_bitcmp1_b64"},
    {16, "s_setvskip"},
};
const Names kSopp = {
    {0, "s_nop"}, {1, "s_endpgm"}, {2, "s_branch"}, {4, "s_cbranch_scc0"},
    {5, "s_cbranch_scc1"}, {6, "s_cbranch_vccz"}, {7, "s_cbranch_vccnz"},
    {8, "s_cbranch_execz"}, {9, "s_cbranch_execnz"}, {10, "s_barrier"}, {12, "s_waitcnt"},
    {13, "s_sethalt"}, {14, "s_sleep"}, {15, "s_setprio"}, {16, "s_sendmsg"},
    {17, "s_sendmsghalt"}, {18, "s_trap"}, {19, "s_icache_inv"}, {20, "s_incperflevel"},
    {21, "s_decperflevel"}, {22, "s_ttracedata"}, {23, "s_cbranch_cdbgsys"},
    {24, "s_cbranch_cdbguser"}, {25, "s_cbranch_cdbgsys_or_user"},
    {26, "s_cbranch_cdbgsys_and_user"},
};
const Names kSmrd = {
    {0, "s_load_dword"}, {1, "s_load_dwordx2"}, {2, "s_load_dwordx4"}, {3, "s_load_dwordx8"},
    {4, "s_load_dwordx16"}, {8, "s_buffer_load_dword"}, {9, "s_buffer_load_dwordx2"},
    {10, "s_buffer_load_dwordx4"}, {11, "s_buffer_load_dwordx8"},
    {12, "s_buffer_load_dwordx16"}, {29, "s_dcache_inv_vol"}, {30, "s_memtime"},
    {31, "s_dcache_inv"},
};
const Names kVop2 = {
    {0, "v_cndmask_b32"}, {1, "v_readlane_b32"}, {2, "v_writelane_b32"}, {3, "v_add_f32"},
    {4, "v_sub_f32"}, {5, "v_subrev_f32"}, {6, "v_mac_legacy_f32"}, {7, "v_mul_legacy_f32"},
    {8, "v_mul_f32"}, {9, "v_mul_i32_i24"}, {10, "v_mul_hi_i32_i24"}, {11, "v_mul_u32_u24"},
    {12, "v_mul_hi_u32_u24"}, {13, "v_min_legacy_f32"}, {14, "v_max_legacy_f32"},
    {15, "v_min_f32"}, {16, "v_max_f32"}, {17, "v_min_i32"}, {18, "v_max_i32"},
    {19, "v_min_u32"}, {20, "v_max_u32"}, {21, "v_lshr_b32"}, {22, "v_lshrrev_b32"},
    {23, "v_ashr_i32"}, {24, "v_ashrrev_i32"}, {25, "v_lshl_b32"}, {26, "v_lshlrev_b32"},
    {27, "v_and_b32"}, {28, "v_or_b32"}, {29, "v_xor_b32"}, {30, "v_bfm_b32"},
    {31, "v_mac_f32"}, {32, "v_madmk_f32"}, {33, "v_madak_f32"}, {34, "v_bcnt_u32_b32"},
    {35, "v_mbcnt_lo_u32_b32"}, {36, "v_mbcnt_hi_u32_b32"}, {37, "v_add_i32"},
    {38, "v_sub_i32"}, {39, "v_subrev_i32"}, {40, "v_addc_u32"}, {41, "v_subb_u32"},
    {42, "v_subbrev_u32"}, {43, "v_ldexp_f32"}, {44, "v_cvt_pkaccum_u8_f32"},
    {45, "v_cvt_pknorm_i16_f32"}, {46, "v_cvt_pknorm_u16_f32"}, {47, "v_cvt_pkrtz_f16_f32"},
    {48, "v_cvt_pk_u16_u32"}, {49, "v_cvt_pk_i16_i32"},
};
const Names kVop1 = {
    {0, "v_nop"}, {1, "v_mov_b32"}, {2, "v_readfirstlane_b32"}, {3, "v_cvt_i32_f64"},
    {4, "v_cvt_f64_i32"}, {5, "v_cvt_f32_i32"}, {6, "v_cvt_f32_u32"}, {7, "v_cvt_u32_f32"},
    {8, "v_cvt_i32_f32"}, {9, "v_mov_fed_b32"}, {10, "v_cvt_f16_f32"}, {11, "v_cvt_f32_f16"},
    {12, "v_cvt_rpi_i32_f32"}, {13, "v_cvt_flr_i32_f32"}, {14, "v_cvt_off_f32_i4"},
    {15, "v_cvt_f32_f64"}, {16, "v_cvt_f64_f32"}, {17, "v_cvt_f32_ubyte0"},
    {18, "v_cvt_f32_ubyte1"}, {19, "v_cvt_f32_ubyte2"}, {20, "v_cvt_f32_ubyte3"},
    {21, "v_cvt_u32_f64"}, {22, "v_cvt_f64_u32"}, {23, "v_trunc_f64"}, {24, "v_ceil_f64"},
    {25, "v_rndne_f64"}, {26, "v_floor_f64"}, {32, "v_fract_f32"}, {33, "v_trunc_f32"},
    {34, "v_ceil_f32"}, {35, "v_rndne_f32"}, {36, "v_floor_f32"}, {37, "v_exp_f32"},
    {38, "v_log_clamp_f32"}, {39, "v_log_f32"}, {40, "v_rcp_clamp_f32"},
    {41, "v_rcp_legacy_f32"}, {42, "v_rcp_f32"}, {43, "v_rcp_iflag_f32"},
    {44, "v_rsq_clamp_f32"}, {45, "v_rsq_legacy_f32"}, {46, "v_rsq_f32"}, {47, "v_rcp_f64"},
    {48, "v_rcp_clamp_f64"}, {49, "v_rsq_f64"}, {50, "v_rsq_clamp_f64"}, {51, "v_sqrt_f32"},
    {52, "v_sqrt_f64"}, {53, "v_sin_f32"}, {54, "v_cos_f32"}, {55, "v_not_b32"},
    {56, "v_bfrev_b32"}, {57, "v_ffbh_u32"}, {58, "v_ffbl_b32"}, {59, "v_ffbh_i32"},
    {60, "v_frexp_exp_i32_f64"}, {61, "v_frexp_mant_f64"}, {62, "v_fract_f64"},
    {63, "v_frexp_exp_i32_f32"}, {64, "v_frexp_mant_f32"}, {65, "v_clrexcp"},
    {66, "v_movreld_b32"}, {67, "v_movrels_b32"}, {68, "v_movrelsd_b32"},
    {69, "v_log_legacy_f32"}, {70, "v_exp_legacy_f32"},
};
const Names kVop3Only = {
    {320, "v_mad_legacy_f32"}, {321, "v_mad_f32"}, {322, "v_mad_i32_i24"},
    {323, "v_mad_u32_u24"}, {324, "v_cubeid_f32"}, {325, "v_cubesc_f32"},
    {326, "v_cubetc_f32"}, {327, "v_cubema_f32"}, {328, "v_bfe_u32"}, {329, "v_bfe_i32"},
    {330, "v_bfi_b32"}, {331, "v_fma_f32"}, {332, "v_fma_f64"}, {333, "v_lerp_u8"},
    {334, "v_alignbit_b32"}, {335, "v_alignbyte_b32"}, {336, "v_mullit_f32"},
    {337, "v_min3_f32"}, {338, "v_min3_i32"}, {339, "v_min3_u32"}, {340, "v_max3_f32"},
    {341, "v_max3_i32"}, {342, "v_max3_u32"}, {343, "v_med3_f32"}, {344, "v_med3_i32"},
    {345, "v_med3_u32"}, {346, "v_sad_u8"}, {347, "v_sad_hi_u8"}, {348, "v_sad_u16"},
    {349, "v_sad_u32"}, {350, "v_cvt_pk_u8_f32"}, {351, "v_div_fixup_f32"},
    {352, "v_div_fixup_f64"}, {353, "v_lshl_b64"}, {354, "v_lshr_b64"}, {355, "v_ashr_i64"},
    {356, "v_add_f64"}, {357, "v_mul_f64"}, {358, "v_min_f64"}, {359, "v_max_f64"},
    {360, "v_ldexp_f64"}, {361, "v_mul_lo_u32"}, {362, "v_mul_hi_u32"},
    {363, "v_mul_lo_i32"}, {364, "v_mul_hi_i32"}, {365, "v_div_scale_f32"},
    {366, "v_div_scale_f64"}, {367, "v_div_fmas_f32"}, {368, "v_div_fmas_f64"},
    {369, "v_msad_u8"}, {370, "v_qsad_pk_u16_u8"}, {371, "v_mqsad_pk_u16_u8"},
    {372, "v_trig_preop_f64"}, {373, "v_mqsad_u32_u8"}, {374, "v_mad_u64_u32"},
    {375, "v_mad_i64_i32"},
};
const Names kVintrp = {{0, "v_interp_p1_f32"}, {1, "v_interp_p2_f32"}, {2, "v_interp_mov_f32"}};
const Names kDs = {
    {0, "ds_add_u32"}, {1, "ds_sub_u32"}, {2, "ds_rsub_u32"}, {3, "ds_inc_u32"},
    {4, "ds_dec_u32"}, {5, "ds_min_i32"}, {6, "ds_max_i32"}, {7, "ds_min_u32"},
    {8, "ds_max_u32"}, {9, "ds_and_b32"}, {10, "ds_or_b32"}, {11, "ds_xor_b32"},
    {12, "ds_mskor_b32"}, {13, "ds_write_b32"}, {14, "ds_write2_b32"},
    {15, "ds_write2st64_b32"}, {16, "ds_cmpst_b32"}, {17, "ds_cmpst_f32"}, {18, "ds_min_f32"},
    {19, "ds_max_f32"}, {20, "ds_nop"}, {30, "ds_write_b8"}, {31, "ds_write_b16"},
    {32, "ds_add_rtn_u32"}, {33, "ds_sub_rtn_u32"}, {34, "ds_rsub_rtn_u32"},
    {35, "ds_inc_rtn_u32"}, {36, "ds_dec_rtn_u32"}, {37, "ds_min_rtn_i32"},
    {38, "ds_max_rtn_i32"}, {39, "ds_min_rtn_u32"}, {40, "ds_max_rtn_u32"},
    {41, "ds_and_rtn_b32"}, {42, "ds_or_rtn_b32"}, {43, "ds_xor_rtn_b32"},
    {44, "ds_mskor_rtn_b32"}, {45, "ds_wrxchg_rtn_b32"}, {46, "ds_wrxchg2_rtn_b32"},
    {47, "ds_wrxchg2st64_rtn_b32"}, {48, "ds_cmpst_rtn_b32"}, {49, "ds_cmpst_rtn_f32"},
    {50, "ds_min_rtn_f32"}, {51, "ds_max_rtn_f32"}, {52, "ds_wrap_rtn_b32"},
    {53, "ds_swizzle_b32"}, {54, "ds_read_b32"}, {55, "ds_read2_b32"},
    {56, "ds_read2st64_b32"}, {57, "ds_read_i8"}, {58, "ds_read_u8"}, {59, "ds_read_i16"},
    {60, "ds_read_u16"}, {61, "ds_consume"}, {62, "ds_append"}, {63, "ds_ordered_count"},
    {64, "ds_add_u64"}, {65, "ds_sub_u64"}, {66, "ds_rsub_u64"}, {67, "ds_inc_u64"},
    {68, "ds_dec_u64"}, {69, "ds_min_i64"}, {70, "ds_max_i64"}, {71, "ds_min_u64"},
    {72, "ds_max_u64"}, {73, "ds_and_b64"}, {74, "ds_or_b64"}, {75, "ds_xor_b64"},
    {76, "ds_mskor_b64"}, {77, "ds_write_b64"}, {78, "ds_write2_b64"},
    {79, "ds_write2st64_b64"}, {80, "ds_cmpst_b64"}, {81, "ds_cmpst_f64"}, {82, "ds_min_f64"},
    {83, "ds_max_f64"}, {118, "ds_read_b64"}, {119, "ds_read2_b64"},
    {120, "ds_read2st64_b64"}, {222, "ds_write_b96"}, {223, "ds_write_b128"},
    {254, "ds_read_b96"}, {255, "ds_read_b128"},
};
const Names kMubuf = {
    {0, "buffer_load_format_x"}, {1, "buffer_load_format_xy"}, {2, "buffer_load_format_xyz"},
    {3, "buffer_load_format_xyzw"}, {4, "buffer_store_format_x"},
    {5, "buffer_store_format_xy"}, {6, "buffer_store_format_xyz"},
    {7, "buffer_store_format_xyzw"}, {8, "buffer_load_ubyte"}, {9, "buffer_load_sbyte"},
    {10, "buffer_load_ushort"}, {11, "buffer_load_sshort"}, {12, "buffer_load_dword"},
    {13, "buffer_load_dwordx2"}, {14, "buffer_load_dwordx4"}, {15, "buffer_load_dwordx3"},
    {24, "buffer_store_byte"}, {26, "buffer_store_short"}, {28, "buffer_store_dword"},
    {29, "buffer_store_dwordx2"}, {30, "buffer_store_dwordx4"}, {31, "buffer_store_dwordx3"},
    {48, "buffer_atomic_swap"}, {49, "buffer_atomic_cmpswap"}, {50, "buffer_atomic_add"},
    {51, "buffer_atomic_sub"}, {53, "buffer_atomic_smin"}, {54, "buffer_atomic_umin"},
    {55, "buffer_atomic_smax"}, {56, "buffer_atomic_umax"}, {57, "buffer_atomic_and"},
    {58, "buffer_atomic_or"}, {59, "buffer_atomic_xor"}, {60, "buffer_atomic_inc"},
    {61, "buffer_atomic_dec"}, {62, "buffer_atomic_fcmpswap"}, {63, "buffer_atomic_fmin"},
    {64, "buffer_atomic_fmax"}, {112, "buffer_wbinvl1_vol"}, {113, "buffer_wbinvl1"},
};
const Names kMtbuf = {
    {0, "tbuffer_load_format_x"}, {1, "tbuffer_load_format_xy"},
    {2, "tbuffer_load_format_xyz"}, {3, "tbuffer_load_format_xyzw"},
    {4, "tbuffer_store_format_x"}, {5, "tbuffer_store_format_xy"},
    {6, "tbuffer_store_format_xyz"}, {7, "tbuffer_store_format_xyzw"},
};
const Names kMimg = {
    {0, "image_load"}, {1, "image_load_mip"}, {2, "image_load_pck"}, {3, "image_load_pck_sgn"},
    {4, "image_load_mip_pck"}, {5, "image_load_mip_pck_sgn"}, {8, "image_store"},
    {9, "image_store_mip"}, {10, "image_store_pck"}, {11, "image_store_mip_pck"},
    {14, "image_get_resinfo"}, {15, "image_atomic_swap"}, {16, "image_atomic_cmpswap"},
    {17, "image_atomic_add"}, {18, "image_atomic_sub"}, {20, "image_atomic_smin"},
    {21, "image_atomic_umin"}, {22, "image_atomic_smax"}, {23, "image_atomic_umax"},
    {24, "image_atomic_and"}, {25, "image_atomic_or"}, {26, "image_atomic_xor"},
    {27, "image_atomic_inc"}, {28, "image_atomic_dec"}, {32, "image_sample"},
    {33, "image_sample_cl"}, {34, "image_sample_d"}, {35, "image_sample_d_cl"},
    {36, "image_sample_l"}, {37, "image_sample_b"}, {38, "image_sample_b_cl"},
    {39, "image_sample_lz"}, {40, "image_sample_c"}, {41, "image_sample_c_cl"},
    {42, "image_sample_c_d"}, {43, "image_sample_c_d_cl"}, {44, "image_sample_c_l"},
    {45, "image_sample_c_b"}, {46, "image_sample_c_b_cl"}, {47, "image_sample_c_lz"},
    {48, "image_sample_o"}, {49, "image_sample_cl_o"}, {50, "image_sample_d_o"},
    {51, "image_sample_d_cl_o"}, {52, "image_sample_l_o"}, {53, "image_sample_b_o"},
    {54, "image_sample_b_cl_o"}, {55, "image_sample_lz_o"}, {56, "image_sample_c_o"},
    {57, "image_sample_c_cl_o"}, {58, "image_sample_c_d_o"}, {59, "image_sample_c_d_cl_o"},
    {60, "image_sample_c_l_o"}, {61, "image_sample_c_b_o"}, {62, "image_sample_c_b_cl_o"},
    {63, "image_sample_c_lz_o"}, {64, "image_gather4"}, {65, "image_gather4_cl"},
    {68, "image_gather4_l"}, {69, "image_gather4_b"}, {70, "image_gather4_b_cl"},
    {71, "image_gather4_lz"}, {72, "image_gather4_c"}, {73, "image_gather4_c_cl"},
    {76, "image_gather4_c_l"}, {77, "image_gather4_c_b"}, {78, "image_gather4_c_b_cl"},
    {79, "image_gather4_c_lz"}, {80, "image_gather4_o"}, {81, "image_gather4_cl_o"},
    {84, "image_gather4_l_o"}, {85, "image_gather4_b_o"}, {86, "image_gather4_b_cl_o"},
    {87, "image_gather4_lz_o"}, {88, "image_gather4_c_o"}, {89, "image_gather4_c_cl_o"},
    {92, "image_gather4_c_l_o"}, {93, "image_gather4_c_b_o"}, {94, "image_gather4_c_b_cl_o"},
    {95, "image_gather4_c_lz_o"}, {96, "image_get_lod"}, {104, "image_sample_cd"},
    {105, "image_sample_cd_cl"}, {106, "image_sample_c_cd"}, {107, "image_sample_c_cd_cl"},
    {108, "image_sample_cd_o"}, {109, "image_sample_cd_cl_o"}, {110, "image_sample_c_cd_o"},
    {111, "image_sample_c_cd_cl_o"},
};
const char* const kCmpF[16] = {"f", "lt", "eq", "le", "gt", "lg", "ge", "o",
                               "u", "nge", "nlg", "ngt", "nle", "neq", "nlt", "tru"};
const char* const kCmpI[8] = {"f", "lt", "eq", "le", "gt", "ne", "ge", "t"};

const char* Find(const Names& n, uint32_t op) {
  auto it = n.find(op);
  return it == n.end() ? nullptr : it->second;
}

// VOPC: v_cmp(x)(s)_<cond>_<type>, with v_cmp(x)_class_f32/f64.
const char* VopcName(uint32_t op) {
  static std::map<uint32_t, std::string> names = [] {
    std::map<uint32_t, std::string> m;
    const std::pair<uint32_t, const char*> floats[] = {
        {0x00, "v_cmp_%s_f32"}, {0x10, "v_cmpx_%s_f32"}, {0x20, "v_cmp_%s_f64"},
        {0x30, "v_cmpx_%s_f64"}, {0x40, "v_cmps_%s_f32"}, {0x50, "v_cmpsx_%s_f32"},
        {0x60, "v_cmps_%s_f64"}, {0x70, "v_cmpsx_%s_f64"}};
    char buf[48];
    for (const auto& [base, fmt] : floats)
      for (uint32_t c = 0; c < 16; c++) {
        std::snprintf(buf, sizeof buf, fmt, kCmpF[c]);
        m[base + c] = buf;
      }
    const std::pair<uint32_t, const char*> ints[] = {
        {0x80, "v_cmp_%s_i32"}, {0x90, "v_cmpx_%s_i32"}, {0xA0, "v_cmp_%s_i64"},
        {0xB0, "v_cmpx_%s_i64"}, {0xC0, "v_cmp_%s_u32"}, {0xD0, "v_cmpx_%s_u32"},
        {0xE0, "v_cmp_%s_u64"}, {0xF0, "v_cmpx_%s_u64"}};
    for (const auto& [base, fmt] : ints)
      for (uint32_t c = 0; c < 8; c++) {
        std::snprintf(buf, sizeof buf, fmt, kCmpI[c]);
        m[base + c] = buf;
      }
    m[0x88] = "v_cmp_class_f32";
    m[0x98] = "v_cmpx_class_f32";
    m[0xA8] = "v_cmp_class_f64";
    m[0xB8] = "v_cmpx_class_f64";
    return m;
  }();
  auto it = names.find(op);
  return it == names.end() ? nullptr : it->second.c_str();
}

const char* const kFmtNames[] = {"sop2", "sopk", "sop1", "sopc", "sopp", "smrd", "vop2", "vop1",
                                 "vopc", "vop3", "vintrp", "ds", "mubuf", "mtbuf", "mimg", "exp"};

}  // namespace

bool Decode(const uint32_t* code, size_t avail, Instr* out) {
  if (!avail) return false;
  const uint32_t w = code[0];
  Instr i{};
  i.w0 = w;
  i.dwords = 1;
  bool lit = false;
  if (w >> 25 == 0x3F) {  // VOP2's opcodes 62 and 63
    i.fmt = Fmt::Vop1;
    i.op = uint16_t((w >> 9) & 0xFF);
    lit = (w & 0x1FF) == kLiteral;
  } else if (w >> 25 == 0x3E) {
    i.fmt = Fmt::Vopc;
    i.op = uint16_t((w >> 17) & 0xFF);
    lit = (w & 0x1FF) == kLiteral;
  } else if (w >> 31 == 0) {
    i.fmt = Fmt::Vop2;
    i.op = uint16_t((w >> 25) & 0x3F);
    lit = (w & 0x1FF) == kLiteral || i.op == 32 || i.op == 33;
  } else if (w >> 23 == 0x17F) {
    i.fmt = Fmt::Sopp;
    i.op = uint16_t((w >> 16) & 0x7F);
  } else if (w >> 23 == 0x17E) {
    i.fmt = Fmt::Sopc;
    i.op = uint16_t((w >> 16) & 0x7F);
    lit = (w & 0xFF) == kLiteral || ((w >> 8) & 0xFF) == kLiteral;
  } else if (w >> 23 == 0x17D) {
    i.fmt = Fmt::Sop1;
    i.op = uint16_t((w >> 8) & 0xFF);
    lit = (w & 0xFF) == kLiteral;
  } else if (w >> 28 == 0xB) {
    i.fmt = Fmt::Sopk;
    i.op = uint16_t((w >> 23) & 0x1F);
    lit = i.op == 21;
  } else if (w >> 30 == 2) {
    i.fmt = Fmt::Sop2;
    i.op = uint16_t((w >> 23) & 0x7F);
    lit = (w & 0xFF) == kLiteral || ((w >> 8) & 0xFF) == kLiteral;
  } else if (w >> 27 == 0x18) {
    i.fmt = Fmt::Smrd;
    i.op = uint16_t((w >> 22) & 0x1F);
    lit = !(w & 0x100) && (w & 0xFF) == kLiteral;
  } else {
    switch (w >> 26) {
      case 0x34: i.fmt = Fmt::Vop3; i.op = uint16_t((w >> 17) & 0x1FF); break;
      case 0x32: i.fmt = Fmt::Vintrp; i.op = uint16_t((w >> 16) & 3); break;
      case 0x36: i.fmt = Fmt::Ds; i.op = uint16_t((w >> 18) & 0xFF); break;
      case 0x38: i.fmt = Fmt::Mubuf; i.op = uint16_t((w >> 18) & 0x7F); break;
      case 0x3A: i.fmt = Fmt::Mtbuf; i.op = uint16_t((w >> 16) & 7); break;
      case 0x3C: i.fmt = Fmt::Mimg; i.op = uint16_t((w >> 18) & 0x7F); break;
      case 0x3E: i.fmt = Fmt::Exp; i.op = 0; break;
      default: return false;
    }
    if (i.fmt != Fmt::Vintrp) {
      if (avail < 2) return false;
      i.w1 = code[1];
      i.dwords = 2;
    }
  }
  if (lit) {
    if (avail < size_t(i.dwords) + 1) return false;
    i.literal = code[i.dwords];
    i.literal_used = true;
    i.dwords++;
  }
  *out = i;
  return true;
}

const char* Name(const Instr& i) {
  switch (i.fmt) {
    case Fmt::Sop2: return Find(kSop2, i.op);
    case Fmt::Sopk: return Find(kSopk, i.op);
    case Fmt::Sop1: return Find(kSop1, i.op);
    case Fmt::Sopc: return Find(kSopc, i.op);
    case Fmt::Sopp: return Find(kSopp, i.op);
    case Fmt::Smrd: return Find(kSmrd, i.op);
    case Fmt::Vop2: return Find(kVop2, i.op);
    case Fmt::Vop1: return Find(kVop1, i.op);
    case Fmt::Vopc: return VopcName(i.op);
    case Fmt::Vop3:
      if (i.op < 256) return VopcName(i.op);
      if (i.op < 320) return Find(kVop2, i.op - 256u);
      if (i.op < 384) return Find(kVop3Only, i.op);
      return Find(kVop1, i.op - 384u);
    case Fmt::Vintrp: return Find(kVintrp, i.op);
    case Fmt::Ds: return Find(kDs, i.op);
    case Fmt::Mubuf: return Find(kMubuf, i.op);
    case Fmt::Mtbuf: return Find(kMtbuf, i.op);
    case Fmt::Mimg: return Find(kMimg, i.op);
    case Fmt::Exp: return "exp";
  }
  return nullptr;
}

std::string Disassemble(const Instr& i) {
  char buf[96];
  const char* n = Name(i);
  if (n)
    std::snprintf(buf, sizeof buf, "%-24s", n);
  else
    std::snprintf(buf, sizeof buf, "%s_op_%u%*s", kFmtNames[int(i.fmt)], i.op, 12, "");
  std::string s = buf;
  std::snprintf(buf, sizeof buf, " %08X", i.w0);
  s += buf;
  if (i.dwords > 1 && (i.dwords > 2 || !i.literal_used)) {
    std::snprintf(buf, sizeof buf, " %08X", i.w1);
    s += buf;
  }
  if (i.literal_used) {
    std::snprintf(buf, sizeof buf, " lit %08X", i.literal);
    s += buf;
  }
  return s;
}

}  // namespace recomp::orbis::gpu::gcn
