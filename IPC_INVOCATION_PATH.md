# Composite IPC Invocation Path: `pong_ret()` Example

## Overview
This document traces the complete IPC (Inter-Process Communication) invocation path through the kernel for a synchronous invocation from the **ping client** (tests.unit_pingpong) to the **pong server** (pong.pingpong) in the sched_ping_pong system composition.

## System Composition Setup
From [sched_ping_pong.toml](composition_scripts/sched_ping_pong.toml):

```
Client: tests.unit_pingpong (ping component)
Server: pong.pingpong (pong component)
Interface: pong
Function: pong_ret() -> returns 42
```

---

## 1. CLIENT SIDE: Invocation Initiation

### 1.1 Function Call in ping.c
[src/components/implementation/tests/unit_pingpong/ping.c](src/components/implementation/tests/unit_pingpong/ping.c#L27):
```c
ret = pong_ret();  // Client invokes pong_ret()
assert(ret == 42);
```

### 1.2 Generated Interface Header
[src/components/interface/pong/pong.h](src/components/interface/pong/pong.h):
```c
int pong_ret(void);
```

### 1.3 Client Stub Resolution (User-Level Capability - UCAP)

**Assembly Stub** ([src/components/interface/pong/stubs/stubs.S](src/components/interface/pong/stubs/stubs.S)):
```assembly
cos_asm_stub(pong_ret)
```

This macro expands to the x86_64 version ([src/components/lib/stubs/arch/x86_64/cos_asm_stubs.h](src/components/lib/stubs/arch/x86_64/cos_asm_stubs.h#L182)):

```assembly
.text
.weak pong_ret
.globl __cosrt_extern_pong_ret
.type  pong_ret, @function
.type  __cosrt_extern_pong_ret, @function
.align 16

pong_ret:
__cosrt_extern_pong_ret:
    movabs $__cosrt_ucap_pong_ret, %rax    # Load UCAP address into RAX
    callq *INVFN(%rax)                     # Jump to invocation function via UCAP
    retq

.global __cosrt_fast_callgate_pong_ret
.type  __cosrt_fast_callgate_pong_ret, @function
.align 16

__cosrt_fast_callgate_pong_ret:
    # Fast callgate implementation...
    movq    %rsp, %rdx
    andq    $0xfffffffffffe0000, %rdx      # Isolate thread stack base
    movzwq  COS_SIMPLE_STACK_THDID_OFF(%rdx), %r13    # Get thread ID
    shl     $16, %rax
    or      %rax, %r13
    
    # Get invocation stack
    COS_ULINV_GET_INVSTK
    # Switch to kernel MPK domain
    COS_ULINV_SWITCH_DOMAIN(0x01)
    # Push invocation stack entry
    COS_ULINV_PUSH_INVSTK
    # Switch to kernel domain
    COS_ULINV_SWITCH_DOMAIN(0xfffffffe)
    
    # Set invocation token (proof of authorization)
    movabs  $0x0123456789abcdef, %rbp
    
    # Check client authentication token
    movq    $0xdeadbeefdeadbeef, %rax
    cmp     %rax, %r15
    
    # Jump to kernel's server invocation handler
    movabs  $0x1212121212121212, %rax
    movabs  $srv_call_ret_pong_ret, %rcx
    jmpq   *%rax

srv_call_ret_pong_ret:
    movq    %rax, %r8                      # Save return value
    movq    $0xdeadbeefdeadbeef, %r15      # Save server auth token
    # ... return path setup ...
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %rbp
    retq
```

### 1.4 User-Level Capability (UCAP) Structure

**Location in memory** ([src/components/interface/pong/stubs/stubs.S](src/components/interface/pong/stubs/stubs.S)):
```assembly
.section .ucap, "a", @progbits
.globl __cosrt_ucap_pong_ret
__cosrt_ucap_pong_ret:
    .rep UCAP_SZ              # Repeated UCAP_SZ times
    .quad 0                   # 8-byte entries (capability slots)
    .endr
```

The UCAP contains:
- **Capability ID** (allocated by the composer)
- **Invocation function pointer** (INVFN offset)
- **Rights metadata**

For ping → pong in this system:
- ping's component ID: likely 3 (from composition order)
- pong's component ID: likely 4
- Capability in ping's capability table: points to kernel's IPC invocation handler

---

## 2. KERNEL TRANSITION

### 2.1 SYSENTER/Syscall Invocation

The fast callgate path executes a **synchronous system call**:
```assembly
sysenter    # or syscall on newer x86_64
```

This transitions from **User-Level** (ring 3) to **Kernel-Level** (ring 0).

**Location**: [src/platform/x86_64/entry.S](src/platform/x86_64/entry.S)

### 2.2 Kernel IPC Handler Activation

**Entry Point**: [src/platform/x86_64/entry.S](src/platform/x86_64/entry.S#L54-L76)

```assembly
.section __ipc_entry, "ax"
.align 16
.globl sysenter_entry
sysenter_entry:
    swapgs
    movq %gs:KERNEL_STACK_OFFSET, %rsp      # Switch to kernel stack
    SAVE_REGS_ALL                            # Save all user registers
    movq %rsp, %rdi                          # pt_regs as first argument
    call composite_syscall_handler           # Call kernel dispatcher
    swapgs
    # ... return path ...
```

**Dispatcher**: [src/kernel/capinv.c](src/kernel/capinv.c#L1000-L1050) - `composite_syscall_handler()`

The kernel's IPC module receives:
- **RAX**: Capability ID (identifying which invocation this is)
- **RDI/RSI/RDX/RCX**: Arguments (none for pong_ret())
- **R13**: Thread ID + invocation token
- **R15**: Client authentication token
- **RBP**: Invocation token (proof of authorization)

**Dispatcher Logic**:
```c
COS_SYSCALL __attribute__((section("__ipc_entry"))) sword_t 
composite_syscall_handler(struct pt_regs *regs)
{
    capid_t cap = __userregs_getcap(regs);
    struct cap_header *ch = captbl_lkup(ci->captbl, cap);
    
    /* Fast path: synchronous invocation */
    if (likely(ch->type == CAP_SINV)) {
        sinv_call(thd, (struct cap_sinv *)ch, regs, cos_info);
        return 0;
    }
    /* ... other operations (THD, ASND, ARCV, etc) ... */
}
```

### 2.3 Synchronous Invocation (SINV) Dispatch

**Function**: [src/kernel/include/inv.h](src/kernel/include/inv.h#L282) - `sinv_call()`

The `sinv_call()` function performs the actual invocation setup:

```c
static inline void
sinv_call(struct thread *thd, struct cap_sinv *sinvc, struct pt_regs *regs, 
          struct cos_cpu_local_info *cos_info)
{
    /* Validate server component is alive */
    if (unlikely(!ltbl_isalive(&(sinvc->comp_info.liveness)))) {
        __userregs_set(regs, -EFAULT, ...);
        return;
    }
    
    /* Push invocation stack entry (tracks call chain) */
    if (unlikely(thd_invstk_push(thd, &sinvc->comp_info, ip, sp, cos_info))) {
        return;
    }
    
    /* Update page table to server's address space */
    pgtbl_update(&sinvc->comp_info.pgtblinfo);
    
    /* Switch protection domain (MPK on modern x86_64) */
    chal_protdom_write(sinvc->comp_info.pgtblinfo.protdom);
    
    /* Prepare registers for server invocation */
    __userregs_sinvupdate(regs);
    
    /* Set up return context and jump to server's entry function */
    __userregs_setinv(regs, thd->tid | (get_cpuid() << 16), 
                      sinvc->token, sinvc->entry_addr);
}
```

### 2.4 The `cap_sinv` Structure

**Definition**: [src/kernel/include/inv.h](src/kernel/include/inv.h#L22)

```c
struct cap_sinv {
    struct cap_header h;          /* Capability metadata */
    struct comp_info  comp_info;  /* Server component info (pgtbl, protection domain) */
    vaddr_t           entry_addr; /* Address of server invocation handler (__cosrt_s_pong_ret) */
    invtoken_t        token;      /* Authorization token for this invocation channel */
};
```

**Key Field - `entry_addr`**:
- **Points to**: Server's assembly invocation stub (e.g., `__cosrt_s_pong_ret`)
- **Created by**: Composer during system composition
- **Set via**: [crt_sinv_create()](src/components/lib/crt/crt.c#L688) with `s_fn_addr` parameter
- **Corresponds to**: `c_fn_addr` in composer, which is the client-side invocation function

The relationship:
```
Client Side (ping):           Server Side (pong):
c_fn_addr (pong_ret)  -----> entry_addr (__cosrt_s_pong_ret)
│                             │
└─ points to callable         └─ points to assemby stub that
   function in client            receives invocation in server
```

---

## 2.6 Standard Kernel Syscall Stub Locations

The standard kernel syscall stub system consists of multiple layers working together:

### Kernel Entry Points (Architecture-Specific)

**x86_64**:
- **File**: [src/platform/x86_64/entry.S](src/platform/x86_64/entry.S)
- **Entry Symbol**: `sysenter_entry`
- **Section**: `__ipc_entry` (optimized for cache locality and fast-path execution)
- **Purpose**: Low-level assembly that saves user state and calls the C dispatcher

**x86**:
- **File**: [src/platform/i386/entry.S](src/platform/i386/entry.S)
- **Entry Symbol**: `sysenter_entry`
- **Mechanism**: `sysenter` instruction
- **Stack Setup**: Uses `sysexit` for return

### Kernel Dispatcher (Architecture-Independent)

**Primary Dispatcher**:
- **File**: [src/kernel/capinv.c](src/kernel/capinv.c#L1000)
- **Function**: `composite_syscall_handler()`
- **Attributes**: `__attribute__((section("__ipc_entry")))`
- **Purpose**: Main demultiplexer that routes syscalls to appropriate handlers

**Fast Path (IPC Invocations)**:
```c
if (likely(ch->type == CAP_SINV)) {
    sinv_call(thd, (struct cap_sinv *)ch, regs, cos_info);
    return 0;
}
```

### Server-Side Invocation Handlers

The kernel doesn't directly call the server function. Instead, the kernel **sets up the RIP register** to point to the server's invocation handler:

```c
__userregs_setinv(regs, thd->tid | (get_cpuid() << 16), 
                  sinvc->token, sinvc->entry_addr);
```

This means:
1. **RIP is set to**: `sinvc->entry_addr` (which is the `c_fn_addr` from the composer)
2. **The kernel transitions back to user mode** with `sysretq`
3. **Execution begins** at the server's invocation stub (e.g., `__cosrt_s_pong_ret`)

### The `c_fn_addr` / `entry_addr` Relationship

The flow of addresses through the system:

```
Composer (build time)
│
├─ Symbol lookup: finds __cosrt_s_pong_ret in pong binary
├─ Virtual address: 0x3000005d0 (example)
├─ Stores in initargs as "c_fn_addr": "820087280" (hex converted to decimal)
│
└─> Runtime (llbooter initialization)
    │
    ├─ Read from initargs: c_fn_addr = 820087280
    ├─ Call crt_sinv_create(..., c_fn_addr, ...)
    │
    └─> Kernel (during system setup)
        │
        ├─ sinv_activate() creates cap_sinv
        ├─ Sets sinvc->entry_addr = c_fn_addr
        │
        └─> IPC Invocation (runtime)
            │
            ├─ composite_syscall_handler() → sinv_call()
            ├─ __userregs_setinv() loads RIP = sinvc->entry_addr
            ├─ sysretq returns to user mode
            └─> Jumps to server stub: __cosrt_s_pong_ret
```

### Key Files and Functions

| Component | File | Function/Symbol | Purpose |
|-----------|------|-----------------|---------|
| **Entry Point** | [src/platform/x86_64/entry.S](src/platform/x86_64/entry.S) | `sysenter_entry` | Save registers, switch to kernel stack |
| **Dispatcher** | [src/kernel/capinv.c](src/kernel/capinv.c) | `composite_syscall_handler()` | Route syscall to handler |
| **IPC Handler** | [src/kernel/include/inv.h](src/kernel/include/inv.h) | `sinv_call()` | Setup invocation context |
| **Cap Structure** | [src/kernel/include/inv.h](src/kernel/include/inv.h) | `struct cap_sinv` | Holds `entry_addr` |
| **Register Setup** | [src/kernel/include/chal/call_convention.h](src/kernel/include/chal/call_convention.h) | `__userregs_setinv()` | Load RIP, token, etc. |
| **Composer** | [src/composer/src/passes.rs](src/composer/src/passes.rs) | Symbol matching | Find `__cosrt_s_` stubs |
| **CRT** | [src/components/lib/crt/crt.c](src/components/lib/crt/crt.c) | `crt_sinv_create()` | Create SINV capability |

### Slow Path Operations

The dispatcher also handles non-invocation operations:

**Capability Table Operations**:
```c
case CAP_THD:
    ret = cap_thd_op(...)      // Thread dispatch
case CAP_ASND:
    ret = cap_asnd_op(...)     // Asynchronous send
case CAP_ARCV:
    ret = cap_arcv_op(...)     // Async receive
```

**Resource Table Operations** (slowpath):
- Capability table manipulation
- Page table operations
- Memory management

---

---

## 3. SERVER SIDE: Receiving the Invocation

### 3.1 Server Stub Activation

**Assembly Server Stub** (compiler-generated for server registration):
```assembly
__cosrt_s_pong_ret:
    # Get invocation token from stack
    COS_ASM_GET_STACK_INVTOKEN
    
    # Set up stack frame
    mov %rsp, %rbp
    and $~0xf, %rsp          # 16-byte stack alignment for ABI
    xor %rbp, %rbp           # Clear base pointer (end of stack trace)
    
    # No arguments for pong_ret(), so call directly
    callq __cosrt_s_cstub_pong_ret
    
    # Prepare return value in RAX
    mov %rax, %r8            # Save return value
    
    # Prepare return capability
    mov $RET_CAP, %rax       # Return capability ID
    
    # Get return stack
    COS_ASM_RET_STACK
    
    # Return to kernel with SYSENTER
    sysenter
```

### 3.2 C Server Stub

[src/components/interface/pong/stubs/s_stub.c](src/components/interface/pong/stubs/s_stub.c):
```c
// Auto-generated for direct invocation (4 args, 1 return)
// No explicit stub needed - pong_ret() is called directly via the assembly
```

For `pong_ret()` specifically, it's a "fast-path" stub that directly calls the server function without a C wrapper since it has simple calling conventions (0 arguments, 1 return value).

### 3.3 Server Implementation

[src/components/implementation/pong/pingpong/pong.c](src/components/implementation/pong/pingpong/pong.c#L51):
```c
int
pong_ret(void)
{
    assert(state >= PONG_PARINIT);
    return 42;
}
```

The pong component's implementation simply returns 42.

---

## 4. RETURN PATH: Returning to Client

### 4.1 Server Return Preparation

After pong_ret() executes:
- **RAX**: Return value (42)
- **R8**: Copy of return value for kernel processing
- **RBX/RBP/RSI/RDI**: Callee-saved registers (from assembly stub)

### 4.2 Kernel Return Processing

The kernel receives the return via sysenter:
1. Validates server authentication token (R15)
2. Copies return value from RAX/RDX to client thread context
3. Prepares client thread for resumption
4. Updates RIP to return address in client code
5. Switches protection domain back to client (using MPK on modern x86_64)

### 4.3 Client Continuation

The client's `pong_ret()` call returns with:
- **RAX**: 42 (the return value)
- Client execution resumes at the instruction after the invocation

```c
ret = pong_ret();  // <-- Returns here with RAX = 42
assert(ret == 42);  // <-- This assertion passes
```

---

## 5. Complete Invocation Timeline

```
┌─────────────────────────────────────────────────────────────────┐
│                        PING CLIENT (ring 3)                     │
├─────────────────────────────────────────────────────────────────┤
│ ret = pong_ret();                                               │
│   ↓                                                              │
│ movabs $__cosrt_ucap_pong_ret, %rax                             │
│ callq  *INVFN(%rax)         # Jump to fast callgate             │
│   ↓                                                              │
│ [Fast callgate path]                                            │
│   - Load UCAP metadata                                          │
│   - Get invocation stack                                        │
│   - Switch to kernel MPK domain (0x01)                          │
│   - Prepare invocation token & auth token                       │
│   - sysenter / syscall  ────────────────────────────┐           │
│                                                     │           │
│                                        ┌────────────┼───────────┤
│                                        │ (KERNEL)   │ ring 0    │
│                                        ├────────────┼───────────┤
│                                        │ Validate:  │           │
│                                        │  - Cap ID  │           │
│                                        │  - Tokens  │           │
│                                        │  - Rights  │           │
│                                        │     ↓      │           │
│                                        │ Find pong  │           │
│                                        │ thread &   │           │
│                                        │ schedule   │           │
│                                        └────────────┤───────────┤
│                                                     │           │
│                            ┌────────────────────────┘           │
│                            ↓                                    │
│                ┌─────────────────────────────────┐             │
│                │   PONG SERVER (ring 3)          │             │
│                ├─────────────────────────────────┤             │
│                │ __cosrt_s_pong_ret:             │             │
│                │   callq pong_ret()              │             │
│                │                                 │             │
│                │ int pong_ret(void) {            │             │
│                │   return 42;  <-- Execution     │             │
│                │ }                               │             │
│                │   ↓                             │             │
│                │ mov $RET_CAP, %rax              │             │
│                │ sysenter  ─────────────────┐   │             │
│                └─────────────────────────────┼───┘             │
│                                              │                 │
│                                 ┌────────────┼────────────────┤
│                                 │  (KERNEL)  │ ring 0         │
│                                 ├────────────┼────────────────┤
│                                 │ Validate   │                │
│                                 │ server     │                │
│                                 │ return &   │                │
│                                 │ copy value │                │
│                                 │     ↓      │                │
│                                 │ Resume     │                │
│                                 │ client ctx │                │
│                                 └────────────┼────────────────┤
│                                              │                 │
│                            ┌─────────────────┘                 │
│                            ↓                                    │
│ [Resume client invocation path]                                │
│   - RAX = 42 (return value)                                   │
│   - Switch back to client MPK domain                           │
│   - Switch to client stack                                     │
│   - retq                                                       │
│   ↓                                                             │
│ ret = pong_ret();  // <-- Returns here with RAX = 42           │
│ assert(ret == 42);  // PASS                                    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. Key Composite Abstractions

### 6.1 User-Level Capability (UCAP)
- **Purpose**: Provides PLT-like indirection for interface functions
- **Location**: `.ucap` section in binary (read-only data)
- **Contains**: Capability ID, invocation function pointer, metadata
- **Access**: Loaded by address in assembly stub, dereferenced via INVFN offset

### 6.2 Invocation Token
- **Purpose**: Proves authorization for this specific invocation
- **Set by**: Client in RBP before sysenter
- **Validated by**: Kernel to ensure client has rights
- **Prevents**: Unauthorized cross-component calls

### 6.3 Authentication Token  
- **Purpose**: Verifies integrity of client/server interaction
- **Client token**: R15 when entering kernel
- **Server token**: Set by kernel after validating client
- **Used for**: Detecting tampering or replayed invocations

### 6.4 Synchronous Invocation (SINV)
- **Type**: Synchronous - client blocks until server returns
- **Mechanism**: sysenter/syscall with memory protection keys (MPK)
- **Context switch**: Kernel switches from client to server thread
- **Atomicity**: Return from server to client is atomic from component perspective

### 6.5 Memory Protection Keys (MPK)
- **Modern x86_64 optimization**: Avoid TLB flush on domain switch
- **0x01**: User-level invocation domain
- **0xfffffffe**: Kernel domain
- **Benefit**: Sub-microsecond domain transitions

---

## 7. System Composition Dependencies

From [sched_ping_pong.toml](composition_scripts/sched_ping_pong.toml):

```toml
[[components]]
name = "ping"
img  = "tests.unit_pingpong"
deps = [{srv = "pong", interface = "pong"}]       # Depends on pong's interface

[[components]]
name = "pong"
img  = "pong.pingpong"
implements = [{interface = "pong"}]               # Implements pong interface
deps = [{srv = "sched", interface = "init"}]      # Depends on scheduler
```

The composer:
1. **Allocates** a capability ID for ping→pong channel
2. **Initializes** ping's UCAP with this capability ID
3. **Registers** pong's invocation handler in kernel
4. **Sets up** scheduler's invocation entry points

---

## 8. Performance Characteristics

From [src/components/implementation/tests/unit_pingpong/ping.c](src/components/implementation/tests/unit_pingpong/ping.c#L47-L62):

```c
// Measure fast-path invocation
begin = ps_tsc();
for (i = 0; i < ITER; i++) {
    pong_call();  // No-arg invocation
}
end = ps_tsc();
fast_path = (end - begin)/ITER;

// Measure with all arguments and returns
begin = ps_tsc();
for (i = 0; i < ITER; i++) {
    pong_argsrets(0, 0, 0, 0, &r0, &r1);
}
end = ps_tsc();
all_args = (end - begin)/ITER;
```

**Fast-path** (`pong_ret()`): 
- No argument marshaling
- Direct stub invocation
- Minimal kernel processing
- Typically 100-300 cycles on modern hardware

---

## 8.1 Critical Discovery: The `c_fn_addr` Address on x86_64

When examining the bootloader initialization in [llbooter.c](src/components/implementation/no_interface/llbooter/llbooter.c#L397), all invocation entries contain the same `c_fn_addr` value (0x16001D0 in decimal = 23078384):

```c
pong_ret:        c_fn_addr = 23078384
init_exit:       c_fn_addr = 23078384  
capmgr_create_noop: c_fn_addr = 23078384
```

**Key Finding: This address is NOT used on x86_64!**

The `c_fn_addr` is stored in the UCAP structure at initialization ([crt.c](src/components/lib/crt/crt.c#L743)):
```c
*ucap = (struct usr_inv_cap) {
    .invocation_fn = sinv->c_fn_addr,  /* stored but not used on x86_64 */
    .cap_no        = sinv->sinv_cap,
    .alt_fn        = alt_fn,
};
```

However, when the invocation is executed, there are two distinct paths in [cos_component.h](src/components/lib/component/arch/x86_64/cos_component.h#L209):

### **Fast Path (alt_fn available - MPK enabled):**
```c
if (likely(uc->alt_fn)) 
    return (uc->alt_fn)(arg1, arg2, arg3, arg4, &r1, &r2);
```
Directly calls the JIT-compiled fast callgate

### **Slow Path (alt_fn NULL - MPK disabled or not shared VAS):**
```c
return call_cap_op(uc->cap_no, 0, arg1, arg2, arg3, arg4);
```
Uses SYSENTER with the capability number, **completely bypassing the `invocation_fn` field**

**Architecture Specific Behavior:**

The `invocation_fn` field is actually used on **ARM architectures** only, via the ARM-specific function [cos_inv_cap_set()](src/components/lib/component/cos_component.c#L288):

```c
#if defined(__arm__)
CWEAKSYMB vaddr_t
cos_inv_cap_set(struct usr_inv_cap *uc)
{
    set_stk_data(INVCAP_OFFSET, (long)uc);
    return uc->invocation_fn;  // Returns this address for ARM
}
#endif
```

On x86_64, this function doesn't exist, so `invocation_fn` is dead code. The shared bootloader generates the same data structures for both architectures, but x86_64 doesn't use the `invocation_fn` field.

**Implication:** The mystery address 0x16001D0 appearing in all invocations is a bootloader artifact that has no functional impact on x86_64 systems. All x86_64 slow-path invocations route through the kernel via SYSENTER using capability numbers, not direct function addresses.

---

## 9. Summary

The IPC invocation path for `pong_ret()` involves:

1. **Client** calls `pong_ret()` (C library call)
2. **Assembly stub** loads UCAP and jumps to fast callgate
3. **Fast callgate** prepares invocation (tokens, stack, domain)
4. **SYSENTER** transitions to kernel mode
5. **Kernel** validates rights and context-switches to pong thread
6. **Server** executes `pong_ret()` which returns 42
7. **Kernel** validates server and returns value to client
8. **Assembly return** switches back to client domain
9. **Client** receives 42 in RAX and continues execution

This design provides:
- **Isolation**: Components can't directly access each other's memory
- **Authorization**: Capabilities prove rights to invoke
- **Performance**: Fast-path stubs minimize overhead
- **Flexibility**: Kernel can enforce scheduling/timing policies
