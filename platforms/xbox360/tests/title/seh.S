// Structured exception handling as titles' compiler lays it out, for
// title.c's Exceptions(): functions with the prologues it writes (LR saved
// at -8 from r1 on entry, then the registers, then stwu), .pdata bits from
// __xex_pdata_* symbols (the prologue's length in instructions, bit 30,
// bit 31 for a handler), and before a function with a handler the
// handler's address and its data. seh_log (title.c reads it) records what
// the funclets saw.
  .text

// A function's .pdata bits: `prologue` instructions, bit 30, and bit 31
// with a handler.
  .macro PDATA name, prologue, handler=0
  .globl __xex_pdata_\name
__xex_pdata_\name:
  .size __xex_pdata_\name, 0x40000000 | (\handler << 31) | \prologue
  .endm

// reg = the address of sym, as titles build one (lis, addi): in 32-bit
// mode an address above 2 GiB is the sign extension of its low word.
  .macro LA reg, sym
  lis \reg, (\sym - 0x100000000)@ha
  addi \reg, \reg, (\sym - 0x100000000)@l
  .endm

  .macro FUNC name
  .globl \name
  .type \name, @function
\name:
  .endm

// Markers the unwinds must keep: a nonvolatile register's value in each
// frame.
  .set MARK_TRY, 0x3131
  .set MARK_TRY30, 0x3030
  .set MARK_OUTER, 0x0E0E
  .set MARK_OWN, 0x0F0F
  .set MARK_RAISE, 0x5EA5

// seh_raise(code, flags): raises the exception `code` (no parameters);
// returns 7 if a handler continues it, with its own r30 as it set it. Its
// prologue keeps LR in r31, as compiled code does when it has no other use
// for that register.
  .p2align 2
  PDATA seh_raise, 4
  FUNC seh_raise
  std 31, -8(1)
  std 30, -16(1)
  mflr 31
  stwu 1, -0xB0(1)
  stw 3, 0x50(1)
  stw 4, 0x54(1)
  li 0, 0
  stw 0, 0x58(1)
  stw 0, 0x5C(1)
  stw 0, 0x60(1)
  lis 30, MARK_RAISE
  addi 3, 1, 0x50
  bl RtlRaiseException
  lis 0, MARK_RAISE
  li 3, 7
  cmpw 30, 0
  beq 1f
  li 3, -1
1:
  addi 1, 1, 0xB0
  mtlr 31
  ld 31, -8(1)
  ld 30, -16(1)
  blr
  .size seh_raise, .-seh_raise

// seh_try(code): __try { seh_raise(code, 0) } __except (seh_try_filter):
// the __except block returns 0x100 | the code's low byte, -1 if r30, r31 or
// its frame are not what they were. The frame (r1 on entry) goes to
// seh_log[4].
  .p2align 2
  .long __C_specific_handler, seh_try_scopes
  PDATA seh_try, 5, 1
  FUNC seh_try
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  std 30, -24(1)
  stwu 1, -0x70(1)
  lis 30, MARK_TRY30
  lis 31, MARK_TRY
  addi 11, 1, 0x70
  LA 10, seh_log
  stw 11, 16(10)
.Ltry_begin:
  li 4, 0
  bl seh_raise
.Ltry_end:
  b .Ltry_out
.Ltry_except:
  lis 0, MARK_TRY30
  cmpw 30, 0
  bne .Ltry_bad
  lis 0, MARK_TRY
  cmpw 31, 0
  bne .Ltry_bad
  LA 10, seh_log
  lwz 11, 16(10)
  addi 0, 1, 0x70
  cmpw 11, 0
  bne .Ltry_bad
  andi. 3, 3, 0xFF
  ori 3, 3, 0x100
  b .Ltry_out
.Ltry_bad:
  li 3, -1
.Ltry_out:
  addi 1, 1, 0x70
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  ld 30, -24(1)
  blr
  .size seh_try, .-seh_try

// The filter: code ...01 executes the handler, ...03 continues execution,
// others search on. Its frame (r12) goes to seh_log[0].
  .p2align 2
  FUNC seh_try_filter
  lwz 11, 0(3)
  lwz 11, 0(11)
  LA 10, seh_log
  stw 12, 0(10)
  andi. 11, 11, 0xFF
  li 3, 1
  cmpwi 11, 1
  beqlr
  li 3, -1
  cmpwi 11, 3
  beqlr
  li 3, 0
  blr
  .size seh_try_filter, .-seh_try_filter

// seh_outer(code): __try { __try { seh_try(code) } __finally
// (seh_outer_finally) } __except (1): the __except block returns 0x200 |
// the code's low byte (-1 if r30 is not what it was); the normal path runs
// the __finally itself, as compiled code does (r3 0, r12 the frame). Its
// prologue saves through a register save routine and sets r31 as the frame
// pointer; the body moves the stack pointer further (as alloca does)
// before the call, so only r31 locates the frame.
  .p2align 2
  .long __C_specific_handler, seh_outer_scopes
  PDATA seh_outer, 4, 1
  FUNC seh_outer
  mflr 12
  bl save_gpr30_lr
  addi 31, 1, -0x60
  stwu 1, -0x60(1)
  stwu 1, -0x20(1)
  lis 30, MARK_OUTER
.Louter_begin:
  bl seh_try
.Louter_end:
  stw 3, 0x50(31)
  li 3, 0
  addi 12, 31, 0x60
  bl seh_outer_finally
  lwz 3, 0x50(31)
  b .Louter_out
.Louter_except:
  andi. 4, 3, 0xFF
  ori 4, 4, 0x200
  lis 0, MARK_OUTER
  cmpw 30, 0
  li 3, -1
  bne .Louter_out
  mr 3, 4
.Louter_out:
  addi 1, 31, 0x60
  b rest_gpr30_lr
  .size seh_outer, .-seh_outer

// A register save routine and its restore, as compiled code calls them:
// r30, r31 and LR (from r12) below r1.
  .p2align 2
  FUNC save_gpr30_lr
  std 30, -24(1)
  std 31, -16(1)
  stw 12, -8(1)
  blr
  .size save_gpr30_lr, .-save_gpr30_lr
  FUNC rest_gpr30_lr
  ld 30, -24(1)
  ld 31, -16(1)
  lwz 12, -8(1)
  mtlr 12
  blr
  .size rest_gpr30_lr, .-rest_gpr30_lr

// The __finally: counts its runs in seh_log[1], its r3 (abnormal
// termination) in seh_log[2], its frame (r12) in seh_log[3].
  .p2align 2
  FUNC seh_outer_finally
  LA 10, seh_log
  lwz 11, 4(10)
  addi 11, 11, 1
  stw 11, 4(10)
  stw 3, 8(10)
  stw 12, 12(10)
  blr
  .size seh_outer_finally, .-seh_outer_finally

// own_frame(code): a function with a title's own language handler
// (own_handler), which unwinds to the code after the call with r3 0x77,
// as a C++ frame handler continues after a catch block. Returns r3 + 0x300
// (-1 if r31 is not what it was).
  .p2align 2
  .long own_handler, 0
  PDATA own_frame, 4, 1
  FUNC own_frame
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  stwu 1, -0x60(1)
  lis 31, MARK_OWN
  li 4, 0
  bl seh_raise
.Lown_continue:
  lis 0, MARK_OWN
  cmpw 31, 0
  addi 3, 3, 0x300
  beq 1f
  li 3, -1
1:
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  blr
  .size own_frame, .-own_frame

// own_handler(record, frame, context, dc): dispatching, unwinds to
// own_frame's continuation (RtlUnwind(frame, .Lown_continue, record,
// 0x77)); unwinding, searches on. Counts its calls in seh_log[5].
  .p2align 2
  PDATA own_handler, 3
  FUNC own_handler
  mflr 12
  stw 12, -8(1)
  stwu 1, -0x60(1)
  LA 10, seh_log
  lwz 11, 20(10)
  addi 11, 11, 1
  stw 11, 20(10)
  lwz 0, 4(3)
  andi. 0, 0, 6
  bne 1f
  mr 5, 3
  mr 3, 4
  LA 4, .Lown_continue
  li 6, 0x77
  bl RtlUnwind
  trap
1:
  li 3, 1
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  blr
  .size own_handler, .-own_handler

// capture(context): RtlCaptureContext(context) with r7 = 0x7777 << 16;
// seh_log[8] gets its r1 at the call, seh_log[9] the return address.
// Returns 1 if r3 came back as the context's address.
  .p2align 2
  PDATA capture, 4
  FUNC capture
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  stwu 1, -0x60(1)
  mr 31, 3
  lis 7, 0x7777
  LA 10, seh_log
  stw 1, 32(10)
  LA 11, .Lcapture_ret
  stw 11, 36(10)
  bl RtlCaptureContext
.Lcapture_ret:
  xor 3, 3, 31
  cntlzw 3, 3
  srwi 3, 3, 5
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  blr
  .size capture, .-capture

// back_outer(): calls back_inner with its frame; back_handler (its
// language handler) makes the unwind that targets this frame continue
// where back_inner captured its context, as titles' C runtime unwinds
// nested frames. Returns back_inner's result.
  .p2align 2
  .long back_handler, 0
  PDATA back_outer, 4, 1
  FUNC back_outer
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  stwu 1, -0x60(1)
  addi 3, 1, 0x60
  bl back_inner
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  blr
  .size back_outer, .-back_outer

// back_inner(frame): RtlCaptureContext(seh_context) with Iar set to the
// return point, RtlUnwind(frame, return point, 0, 0x44): back_handler puts
// the captured context in place, so RtlUnwind returns here, r3 0x44.
// Returns 0x400 + r3 (-1 if r31 is not what it was).
  .p2align 2
  PDATA back_inner, 4
  FUNC back_inner
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  stwu 1, -0x60(1)
  mr 31, 3
  LA 3, seh_context
  bl RtlCaptureContext
  LA 4, .Lback_ret
  stw 4, 8(3)
  mr 3, 31
  li 5, 0
  li 6, 0x44
  bl RtlUnwind
.Lback_ret:
  addi 0, 3, 0x400
  LA 4, seh_context
  lwz 4, 0x18 + 8 * 31 + 4(4)
  li 3, -1
  cmpw 31, 4
  bne 1f
  mr 3, 0
1:
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  blr
  .size back_inner, .-back_inner

// back_handler(record, frame, context, dc): on the unwind's target frame,
// copies seh_context over the context it is given (seh_log[6] counts it).
// Searches on.
  .p2align 2
  FUNC back_handler
  lwz 0, 4(3)
  andi. 0, 0, 0x20
  beq 2f
  LA 10, seh_log
  lwz 11, 24(10)
  addi 11, 11, 1
  stw 11, 24(10)
  LA 4, seh_context
  li 0, 0xA40 / 8
  mtctr 0
1:
  ld 0, 0(4)
  std 0, 0(5)
  addi 4, 4, 8
  addi 5, 5, 8
  bdnz 1b
2:
  li 3, 1
  blr
  .size back_handler, .-back_handler

// sj_setjmp(buffer): saves r1, the return address, r30 and r31 in the
// buffer and returns 0, as setjmp does.
  .p2align 2
  FUNC sj_setjmp
  mflr 0
  stw 0, 4(3)
  std 1, 8(3)
  std 31, 16(3)
  std 30, 24(3)
  li 3, 0
  blr
  .size sj_setjmp, .-sj_setjmp

// sj_longjmp(buffer, value): back to where sj_setjmp returned, with r3 the
// value: the registers from the buffer, then a blr to the saved return
// address with r1 that frame's.
  .p2align 2
  FUNC sj_longjmp
  ld 31, 16(3)
  ld 30, 24(3)
  lwz 5, 4(3)
  mtlr 5
  ld 1, 8(3)
  mr 3, 4
  blr
  .size sj_longjmp, .-sj_longjmp

// sj_caller(): sj_setjmp on a buffer in its frame; on its first return
// changes r31 and calls sj_deep, which jumps back; the second return gives
// 0x500 + the value (-1 if r31 is not back to the value it had at the
// setjmp).
  .p2align 2
  PDATA sj_caller, 4
  FUNC sj_caller
  mflr 12
  stw 12, -8(1)
  std 31, -16(1)
  stwu 1, -0x80(1)
  lis 31, MARK_OWN
  addi 3, 1, 0x50
  bl sj_setjmp
  cmpwi 3, 0
  bne 1f
  li 31, 0
  addi 3, 1, 0x50
  bl sj_deep
  li 3, -2
  b 2f
1:
  addi 3, 3, 0x500
  lis 0, MARK_OWN
  cmpw 31, 0
  beq 2f
  li 3, -1
2:
  addi 1, 1, 0x80
  lwz 12, -8(1)
  mtlr 12
  ld 31, -16(1)
  blr
  .size sj_caller, .-sj_caller

// sj_deep(buffer): a frame between the setjmp and the longjmp.
  .p2align 2
  PDATA sj_deep, 3
  FUNC sj_deep
  mflr 12
  stw 12, -8(1)
  stwu 1, -0x60(1)
  li 4, 5
  bl sj_longjmp
  addi 1, 1, 0x60
  lwz 12, -8(1)
  mtlr 12
  blr
  .size sj_deep, .-sj_deep

  .data
  .p2align 2
// count, then per scope: begin, end, filter (or __finally), __except.
seh_try_scopes:
  .long 1
  .long .Ltry_begin, .Ltry_end, seh_try_filter, .Ltry_except
seh_outer_scopes:
  .long 2
  .long .Louter_begin, .Louter_end, seh_outer_finally, 0
  .long .Louter_begin, .Louter_end, 1, .Louter_except
  .globl seh_log
seh_log:
  .space 64
  .p2align 4
  .globl seh_context
seh_context:
  .space 0xA40
