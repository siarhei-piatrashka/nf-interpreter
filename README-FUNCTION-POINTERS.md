# Managed function pointers (`delegate*<...>`) in .NET nanoFramework

This document covers C# [function pointers](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/unsafe-code#function-pointers) (`delegate* managed<...>`) in .NET nanoFramework:

- how they are implemented;
- where they behave differently from desktop .NET;
- how they compare with delegates in speed and memory.

> Scope: **managed** function pointers only. Unmanaged pointers (`delegate* unmanaged[...]<...>`) are rejected by the metadata processor at build time.

## 1. Usage

```csharp
// requires <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
static int Add(int a, int b) => a + b;
static T Id<T>(T v) => v;

unsafe
{
    delegate*<int, int, int> add = &Add;   // ldftn: no allocation
    int r = add(2, 3);                      // calli: no allocation

    delegate*<string, string> id = &Id<string>;   // generic instantiation (develop / 2.x only)
    string s = id("x");

    delegate*<int, int, int>[] table = { &Add, &Sub };  // 4 bytes per entry
}
```

What is supported:

- **Targets:** static methods, plus generic methods and methods of generic types (closed, or open over the caller's `T`; develop only).
- **Signatures:** any arity, and any parameter or return type: primitives, structs, generic structs, classes, arrays, `ref`/`out`/`in`, `ref` returns, `void`, nested function pointers.
- **Where they can live:** locals, fields (static and instance), arrays, parameters, and return values. They can be compared (`==`, `!=`), passed around, converted to delegates, and measured with `sizeof`.
- **Calls:** `calli` dispatch to the exact target method.

These match .NET semantics. The exceptions are listed in §4.

## 2. Repositories and branches

Two tracks are implemented:

- **1.x track:** `main` on all three repositories, without generics.
- **2.0 track:** `develop` on all three repositories, with generics.

| Repository | 1.x branch | 2.0 branch | What changes |
|---|---|---|---|
| metadata-processor | `feature/managed-function-pointers` | `feature/managed-function-pointers-develop` | IL rewriting of `calli`, signatures, dependency handling |
| nf-interpreter | `feature/managed-function-pointers` | `feature/managed-function-pointers-develop` | `ldftn`, `ldvirtftn`, `calli`, `sizeof`, signature parser, generic registry |
| CoreLibrary | `feature/managed-function-pointers` | `feature/managed-function-pointers-develop` | Tests only (`Tests/NFUnitTestFunctionPointers`); **no API change** |

No changes are needed to:

- the CoreLibrary runtime API;
- Visual Studio or the debugger;
- the PE format version;
- the C# compiler. Roslyn already emits standard `ldftn` and `calli` IL.

## 3. Implementation

### 3.1 Representation

A managed function pointer is a 32-bit `I4` value on the evaluation stack, in locals, in fields and in array elements:

| Value | Meaning |
|---|---|
| `0` | Null function pointer |
| `CLR_RT_MethodDef_Index` (bit 31 clear) | Non-generic target: (assembly index << 24) \| method row. The assembly index is ≥ 1, so a valid pointer is never 0. |
| `0x80000000 \| n` (develop only) | Generic instantiation: entry `n` in the runtime function-pointer registry (§3.4). |

Because the value is a plain `I4`:

- storing, copying, comparing and passing a function pointer costs exactly as much as an `int`;
- the GC never sees it;
- no heap object is created.

### 3.2 metadata-processor (MDP)

nanoFramework PEs are a compact re-encoding of ECMA-335 metadata. The MDP is what must understand function-pointer types, which appear in signatures as `ELEMENT_TYPE_FNPTR` (0x1B).

1. **Type encoding.**
   - A function-pointer type is written as `DATATYPE_I4` everywhere it can appear: fields, locals, parameters, returns, array elements, generic arguments and TypeSpecs.
   - New `DATATYPE_TYPE_FNPTR = 0x42` (in `nanoCLR_DataType.cs`, mirrored in `nanoCLR_Types.h`). It is used only inside method signatures (parameters and returns), as `[0x42][u16 callsite signature index][I4]`.
   - Older runtimes never see this marker. The MDP emits it only when the assembly actually uses function pointers in a method signature, and such assemblies need a runtime that supports `calli` anyway.
   - Purpose of the marker: two overloads such as `M(delegate*<int,void>)` and `M(delegate*<long,void>)` both encode as `I4`, so without it they would have identical nanoFramework signatures. The marker keeps them distinct, so `MethodRef` resolution across assemblies binds to the correct overload, as in .NET.
2. **`calli` operand.** The IL operand of `calli` is a `StandAloneSig` token. The MDP rewrites it into an index in the nanoFramework signature table (`CodeWriter`, InlineSig). The call-site signature is emitted with `nanoSignaturesTable.GetOrCreateSignatureId(CallSite)`.
3. **Validation.** `EnsureManagedFunctionPointer` rejects unmanaged calling conventions (`unmanaged`, `cdecl`, `stdcall`, `thiscall`, `fastcall`) with a clear build error. It also rejects `vararg`.
4. **Custom modifiers.** `modreq`/`modopt` are stripped (`RemoveCustomModifiers`), because nanoFramework signatures have no modifiers. These include `InAttribute` for `in`, `IsExternalInit`, `OutAttribute`, `IsReadOnlyAttribute` and `CallConv*`. After stripping, `in T` encodes as `ref T`, the same as everywhere else in nanoFramework.
5. **Dependencies.** Types referenced only from function-pointer signatures (return and parameter types) are added to the dependency graph (`nanoAssemblyBuilder.AddTypeDependency`). The minimizer therefore does not drop them.
6. **Develop.**
   - `nanoTypeSpecificationsTable` keeps `FunctionPointerType` TypeSpecs, encoded as `I4`. These are needed, for example, for `List<delegate*<int,void>>`-like generic arguments and arrays of function pointers.
   - `ldftn` of a `MethodSpec` (`&Id<string>`) or of a member of a generic type (`&G<int>.M`) keeps the full token, and the runtime resolves it.
7. **Dumper.** `nanoDumperGenerator` prints `FNPTR` signatures.

Unit tests: `MetadataProcessor.Tests/Core/FunctionPointersTests.cs`, 22 tests on each track.

### 3.3 nf-interpreter: opcodes

| Opcode | Before | After |
|---|---|---|
| `ldftn` / `ldvirtftn` | Created the delegate object right away, assuming a delegate `newobj` would follow | Push an `I4` function pointer (§3.1). `ldvirtftn` resolves the virtual or interface override on the object, as .NET does. |
| `newobj` (delegate ctor) | Only checked the object pushed by `ldftn` | When it finds an `I4`, resolves it (generic registry on develop) and creates the delegate with the same `CLR_RT_HeapBlock_Delegate::CreateInstance` call `ldftn` used before. Delegate objects are unchanged. |
| `calli` | `CLR_E_UNSUPPORTED_INSTRUCTION` | Implemented (below) |
| `sizeof` | Failed on TypeSpecs of primitive types | Handles TypeSpecs of primitive types, so `sizeof(delegate*<void>)` = 4 |

**`calli`** (`Interpreter.cpp`, `OPDEF(CEE_CALLI)`):

1. Read the function-pointer value from the top of the stack. A null value throws `NullReferenceException`.
2. **Fast path (cache hit).**
   - Condition: the triple (assembly, call-site signature index, pointer value) is in the validation cache, and the value is non-generic.
   - The method instance is initialised directly from the `CLR_RT_MethodDef_Index` with `InitializeFromIndex`.
   - Signature validation and token resolution are skipped.
3. **Miss path.**
   - `ResolveFunctionPointer` decodes the value. It checks the assembly and row ranges and the registry bounds, so a forged value cannot index out of bounds.
   - `ValidateCallISignature` (a `NOINLINE` helper) checks the call site against the target:
     - the calling convention is managed default (no vararg or generic call site);
     - the parameter count matches;
     - `this`/static agrees;
     - the target is not abstract;
     - every parameter and the return type are **compatible**, using the rules C# applies to method-group and function-pointer conversions (ECMA-335 *compatible-with*):
       - identical types always match; this is compared structurally;
       - a by-value parameter also matches when there is an implicit reference conversion from the call-site type to the target type (contravariance);
       - a by-value return type also matches when there is an implicit reference conversion from the target type to the call-site type (covariance).
       - Implicit reference conversions are:
         - any reference type to `object`;
         - a class to its base classes and implemented interfaces, and an interface to its base interfaces, via the same `IsInstanceOf` as `castclass`;
         - array covariance for reference element types (`string[]` → `object[]`);
         - an array to `System.Array` and its interfaces.
       - As in C#, none of the following converts:
         - boxing (value type → `object` or interface);
         - a `ref`/`out`/`in` parameter or a `ref` return whose type differs (`ref string` → `ref object`). These are fully supported when the types are identical, for example `delegate*<ref int, void> p = &M;` for `void M(ref int i)`;
         - value-type arrays (`int[]` → `object[]`);
         - nested function-pointer types;
         - generic arguments (`Box<string>` vs `Box<object>`).
     - On 1.x this is `FunctionPointer_MatchCallISignature` in `Interpreter.cpp`, built on `MatchSignatureElement`. On develop, `CLR_RT_TypeSystem::MatchCallISignature` does it with generic contexts (§3.4), for generic and non-generic targets alike.
   - On success the triple is stored in the cache.
4. Pop the pointer and push a new stack frame for the target, exactly like `call`.
   - Arguments are already on the stack in .NET order.
   - Generic context (`genericType`/`methodSpec`) is carried over.
   - For generic types, a pending static constructor is triggered first (generic cctor guard), as with a direct `call`.

**Validation cache.**

- `CLR_RT_TypeSystem::m_calliValidated[NANOCLR_CALLI_VALIDATION_CACHE_SIZE]`: 16 entries by default, direct-mapped, power of two. On 32-bit MCUs an entry is 12 bytes on 1.x and 20 bytes on develop.
- On develop, an entry also records the caller's generic context (`genericType`/`methodSpec`) when the call-site signature used it (a VAR/MVAR was substituted). Such an entry only hits for the same caller instantiation, so `Apply<int>` and `Apply<string>` sharing one call site are validated separately.
- Each entry is set only after full validation.
- It is cleared in `TypeSystem_Initialize` and `TypeSystem_Cleanup`, so it never survives an assembly reload or reboot.
- Collisions only cost a re-validation.
- To disable it, define `NANOCLR_CALLI_VALIDATION_CACHE_SIZE 0`.

**Signature parser** (`TypeSystem.cpp`):

- `CLR_RT_SignatureParser::Advance` understands the `0x42` marker and yields `I4`. It records the call-site signature in `Element`, so overload matching compares function-pointer signatures structurally.
- `CLR_RT_ExecutionEngine::InitializeLocals` skips the marker.
- The diagnostics tracer (`Info.cpp`) prints `calli` operands and `FNPTR` signatures.

### 3.4 nf-interpreter: generics (develop only)

A pointer to a generic instantiation needs a generic context (a TypeSpec for `G<int>.M`, a MethodSpec for `M<string>`), and that context does not fit in a `MethodDef_Index`. These pointers therefore go through a **function-pointer registry**:

- `CLR_RT_TypeSystem::m_functionPointers` is a `platform_malloc`'d array of `CLR_RT_FunctionPointerRecord`. Each record holds the method, the generic type and the MethodSpec, plus the producer context when the instantiation is open.
- `ldftn` of a generic target **interns** the instantiation:
  - Two pointers to the same instantiation always compare equal, as in .NET.
  - Identity is structural, not token-based: `&Id<int>` taken in assembly A and in assembly B, or through different TypeSpec rows, is the same pointer.
  - Native memory is allocated once per distinct instantiation, never per `ldftn` execution.
- **Open instantiations.**
  - Example: inside `Outer<T>`, `&Id<T>` with T = `string` at run time.
  - The producing method's generic context is stored in the record.
  - The pointer resolves exactly as a direct call from that method would: `Id<string>`, not `Id<!!0>`.
- **Fast path.** Non-generic values (bit 31 clear) bypass the registry completely in both `GetFunctionPointer` and `ResolveFunctionPointer`, so the generics support adds nothing to non-generic pointers.
- The registry is freed in `TypeSystem_Cleanup`.

**`calli` validation with generics** (`MatchCallISignature`, `TypeSystem.cpp`):

- **Contexts.**
  - The call-site signature is resolved in the generic context of the calling frame. For example, in `T Apply<T>(delegate*<T, T> f, T x) => f(x)` running as `Apply<double>`, the call site is `double(double)`.
  - The target signature is resolved in the target's context.
  - Open registry records (`&Id<T>` taken inside `Make<T>`) resolve a second level through the producer context stored in the record. `Id<!!0>` produced by `Make<string>` is therefore `string(string)`.
- **Comparison.**
  - Both signatures are walked element by element in lockstep, with VAR/MVAR substituted as they are met.
  - The comparison covers element type, array levels, by-ref, class and, for generic instantiations, the generic definition and every argument.
  - Reference conversions (§3.3) apply only to top-level parameter and return types. The walker tracks which elements are top level: frames of nested function-pointer signatures are marked as nested, and a counter tracks the generic arguments still to come. When two top-level types are compatible but not identical, the generic arguments of both are consumed without comparison. A generic instantiation converts to `object` and to the non-generic base types and interfaces of its definition. A class-constrained `T` converts to `object` once substituted.
  - Nested function-pointer signatures are compared too, each in the generic context of the element that contains it.
- **Bounded native stack.**
  - The walker is iterative: substitutions and nested function-pointer signatures push frames on a fixed stack of 8 per side, with no recursion.
  - The comparison function uses 960 bytes of native stack on Cortex-M4, about 1.3 KB at peak including its callees (generic argument skipping, `IsInstanceOf` for reference conversions).
  - A signature deeper than that is rejected. That means more than 7 levels of `delegate*<delegate*<...>>` nesting, fewer when generic substitution is involved.
- **Fail closed.** Anything that cannot be resolved or compared throws `CLR_E_WRONG_TYPE` and is never assumed to match:
  - a generic parameter without a context;
  - a nesting that is too deep;
  - a malformed signature;
  - a class given only as a TypeSpec token.
- **Result.** A forged pointer cannot be called with an incompatible signature, whether it targets a closed instantiation, an open one or a non-generic method (`(delegate*<int, string>)(void*)Make<string>()` throws).

**Bonus fix.** Develop's `MethodDef ResolveToken` ignored array levels when it resolved closed generic types, for example `G<int[]>` vs `G<int>`. Function-pointer identity relies on that resolution, so this was fixed. The fix also corrects direct calls.

### 3.5 Zero breaking changes

1. **Old PEs on the new runtime.** Without the `0x42` marker and `calli`, an existing assembly executes the same opcodes. `ldftn` followed by a delegate `newobj` still creates an identical delegate object.
2. **New MDP on code without function pointers.** The PE is byte-identical to the one the upstream MDP produces. This was verified on CoreLibrary, the test suites and the E2E harness (`#if NOFNPTR` builds).
3. **Matrix.** {upstream MDP, new MDP} × {upstream CLR, new CLR}: all combinations pass for code without function pointers (8/8 on 1.x, 9/9 on develop).
4. **Regression.** The upstream test suites give identical pass counts on the patched runtime vs upstream:

   | Suite | Tests passed (patched = upstream) |
   |---|---|
   | Array | 147 |
   | Struct | 28 |
   | Interface | 97 |
   | Delegates | 46 |
   | Types | 211 |
   | StatementsTests | 144 |
   | BasicConcepts | 158 |
   | GenericStatic | 13 |
   | Nullable | 1 |
   | MemoryExtensions | 12 |
   | ReflectionMember | 4 |
   | InitLocal | 4 |
   | Monitor | 17 |
   | Classes, per file | Constructor 44, Consts 33, Declaration 24, Destructor 3, Event 1, Field 33, Generic 13, HashCode 3, Indexer 30, Members 63, Methods 113, Operator 40, Properties 48, Static 9 |

5. **No public API, wire protocol or PE header changes.**
6. **Old runtime with a new assembly that uses function pointers:** `calli` fails with `CLR_E_UNSUPPORTED_INSTRUCTION`, as it did before. Such code could not run before either, so no working program is broken.

## 4. Differences from desktop .NET

| Topic | .NET (CoreCLR) | nanoFramework |
|---|---|---|
| Size and representation | Native pointer (`nint`), 4 or 8 bytes | Always a 4-byte `I4` token (§3.1); `sizeof(delegate*<void>) == 4` |
| `typeof(delegate*<int,void>)` and reflection | `FunctionPointerType` metadata; `Type.IsFunctionPointer` (NET 8+) | `typeof(int)`: the type is erased to `Int32` in metadata. |
| Conversion to and from `IntPtr`/`void*` | Allowed and meaningful (interop) | Casts compile, but the value is a token, not an address. Do not pass it to native code. Locals of pointer type (`void* p = f;`) are rejected by the MDP, as for any pointer type in nanoFramework. Cast inline instead: `(delegate*<int,int>)(void*)f`. |
| Unmanaged pointers (`delegate* unmanaged`) | Supported | Not supported; build error from MDP |
| Generic targets | Supported | Develop (2.x) only. On 1.x, generic code is not supported in general. A `calli` whose call site or target depends on a generic method that was called with another method's type parameter (`A<T>() => B<T>()`, the MethodSpec `B<!!0>` stays open at run time) is rejected with `CLR_E_WRONG_TYPE`. The develop runtime cannot resolve such contexts in general: `v.ToString()` on `U` inside `B<U>` already asserts upstream. Generic *type* parameters (`G<T>.M() => Apply<T>(...)`) are fully supported. |
| `in` / `ref readonly` parameters | Supported | Supported (encoded as `ref`). Needs `IsReadOnlyAttribute`/`InAttribute` in corlib, which CoreLibrary 2.x provides. |
| Custom modifiers in signatures | Part of the signature identity | Stripped. Two overloads that differ only by modifiers would collide, as for normal methods in nanoFramework. |
| `calli` signature check | None: a mismatched forged pointer is undefined behaviour or a crash | Validated on first use per (site, target). Conversions C# allows are accepted, including reference-type variance, except reference conversions to generic types (next row). A forged pointer with an incompatible signature throws `System.Exception` (`CLR_E_WRONG_TYPE`) instead of corrupting memory. A forged pointer that relies on a conversion C# rejects but CoreCLR happens to execute (e.g. `object` → `string` parameter) also throws. |
| Reference conversions to generic types | Base types and interfaces that are generic instantiations, plus variant generic interfaces (`IEnumerable<string>` → `IEnumerable<object>`) | Not accepted (throws), as for nanoFramework casts: a generic target type has to be identical. |
| Null pointer call | Access violation → `NullReferenceException` | `NullReferenceException` |
| Instance (`HASTHIS`) `calli` | IL-only (not expressible in C#) | IL-only as well. Validated like any other `calli` (`this`/static and arity must match). Vararg and generic call sites are rejected at run time (`NotSupportedException`), and unmanaged conventions by the MDP. |
| Equality of generic instantiations | Same pointer per instantiation | Same (interned registry, structural identity across assemblies) |
| Performance model | JIT inlines or devirtualizes; `calli` ≈ a direct call | Interpreter: `calli` ≈ a direct call, faster than a delegate (§5) |
| Roslyn temp spill (`stloc`/`ldloc` of the pointer before `calli`) | Removed by the JIT | Executed: 2 extra IL ops per call (~40 host instructions) |

C# 11+ caches the delegate for a *static method-group conversion* (`D d = Add;`) in a static field, so repeated conversions don't allocate. An explicit `new D(Add)`, lambdas that capture state, and instance method groups always allocate. Function pointers never allocate.

## 5. Performance

Measurement setup:

- Runtime: `nanoFramework.nanoCLR` posix build (`MinSizeRel`, `-Os`, x86-64) of each branch, compared with the same upstream commit.
- Instruction counts: callgrind, with the empty-loop cost subtracted. They are deterministic and independent of the host.
- Wall clock: the same PE on alternating runtimes. It is noisy (±5–15% from code layout) and is included for orientation only.
- Benchmark sources: `harness/bench/Bench.cs` (outside the repositories).

### 5.1 Instructions per operation (callgrind, lower is better)

| Operation | 1.x | 1.x upstream | develop | develop upstream |
|---|---:|---:|---:|---:|
| Direct `call`, 2 args | 635 | 635 | 864 | 862 |
| Direct `call`, 8 args | 1278 | 1277 | 1466 | 1464 |
| Static delegate invoke, 2 args | 755 | 754 | 989 | 987 |
| Instance delegate invoke | 998 | 998 | 1254 | 1252 |
| Delegate invoke, 8 args | 1416 | 1416 | 1609 | 1607 |
| **Function-pointer call, 2 args** | **802** | n/a | **847** | n/a |
| **Function-pointer call, 8 args** | **1446** | n/a | **1449** | n/a |
| `new D2(Add)` + invoke (incl. amortised GC) | 1292 | 1208 | 1772 | 1648 |
| **`&Add` + call** | **952** | n/a | **1060** | n/a |

Observations:

- **Existing code is unaffected.** Calls and delegate invokes differ by ≤ 2 instructions (≤ 0.2%).
- **Function-pointer call vs delegate invoke:**
  - develop: −14% (847 vs 989), and −10% with 8 args;
  - 1.x: +6% vs a static delegate invoke (+2% with 8 args). The Roslyn temp spill (`stloc`/`ldloc`) adds ~40 instructions that the interpreter cannot remove.
  - vs an instance delegate invoke: −20% on 1.x and −32% on develop.
- **On develop, a `calli` costs less than a direct `call`.** The validated cache hit skips the per-call MethodRef/MethodSpec token resolution that `call` performs.
- **Create + call:**
  - `&Add` + call is 26% cheaper than `new D2(Add)` + invoke on 1.x and 40% cheaper on develop.
  - It involves no allocation and no GC.
- **Delegate creation is slightly slower**: +84 instructions on 1.x and +124 on develop. `ldftn` now pushes an `I4` that `newobj` decodes again, where it used to create the delegate directly. That is the only measurable cost to existing code. It affects only `new Delegate(...)`, not invocation, and it is small next to the allocation and GC cost of the delegate itself.

### 5.2 Wall clock (ns per operation, empty loop subtracted)

| Case | 1.x direct | 1.x delegate | 1.x fnptr | dev direct | dev delegate | dev fnptr |
|---|---:|---:|---:|---:|---:|---:|
| 0 args | 29.0 | 40.0 | 33.7 | 43.1 | 60.8 | 44.1 |
| 2 args | 43.1 | 57.4 | 55.4 | 57.4 | 77.4 | 68.8 |
| 4 args | 59.1 | 77.7 | 69.8 | 78.3 | 93.7 | 87.8 |
| 8 args | 121.3 | 136.6 | 127.5 | 132.9 | 153.6 | 148.9 |
| Struct argument and return | 386.3 | 376.2 | 374.5 | 474.8 | 498.6 | 480.7 |
| Generic `Id<int>` | n/a | n/a | n/a | 49.3 | 73.6 | 57.7 |
| Create + call | n/a | 114.2 | **73.5** | n/a | 150.6 | **85.4** |

- Geometric mean of the full benchmark set, patched vs upstream runtime on code without function pointers: −0.15% on 1.x and −3.95% on develop. Both are noise from code layout; the instruction counts agree with that.
- In wall-clock time, function-pointer calls are 0.5–27% faster than delegate invokes. On 1.x the 2-argument case is within noise, which matches the instruction counts.
- Create + call is 36–43% faster than create + invoke of a delegate.

### 5.3 Memory

Measured with `GC.GetTotalMemory`/`Debug.GC` on posix 64-bit:

| What | Delegates | Function pointers |
|---|---:|---:|
| One callable value | 72 B heap object (64-bit host); ~36 B on 1.x / ~48 B on develop on 32-bit MCUs (12-byte heap units) | **0 B** heap (4-byte `I4` in its slot) |
| 200 stored callables | 14,400 B of objects + array | 0 B of objects |
| Array of 200 | `D2[200]` = 4,848 B (64-bit) | `delegate*<...>[200]` = 840 B (same as `int[200]`) |
| Array of 400 | 9,648 B | 1,632 B |
| Array element, 32-bit MCU | 12 B (object-reference HeapBlock) + target object | **4 B** |
| GC pressure per creation | 1 allocation, collected later | none |

Function pointers are values. They never allocate, never need GC scanning, and keep no target object alive.

### 5.4 Cost of the feature (flash and RAM)

| | 1.x | develop |
|---|---:|---:|
| Cortex-M4 code (`-Os`, Thumb-2; Interpreter + TypeSystem + Execution) | +1,048 B (29,413 → 30,461) | +4,450 B (50,060 → 54,510) |
| x86-64 posix `.text` | +1,828 B | +9,176 B |
| Static RAM (`CLR_RT_TypeSystem`, 32-bit) | +192 B (272 → 464; validation cache 16 × 12 B) | +332 B (296 → 628; cache 16 × 20 B + registry pointers) |
| Native stack (Cortex-M4) | `calli` validation within the existing call path | About 1.3 KB peak (960 B comparison frame), constant (no recursion), on the first validation of a (site, target) pair only |
| Dynamic RAM | none | One `CLR_RT_FunctionPointerRecord` per distinct generic instantiation whose address is taken (native heap, freed at reboot) |
| `CLR_RT_HeapBlock_Delegate` | unchanged | unchanged |

Develop's larger code size comes from three things: generic-instantiation identity (interning and structural comparison), producer-context handling, and generic-aware `calli` validation (§3.4). Setting `NANOCLR_CALLI_VALIDATION_CACHE_SIZE 0` saves the cache RAM (192 B on 1.x, 320 B on develop), at the cost of validating on every `calli`.

### 5.5 When to use which

- **Hot loops and dispatch tables** (state machines, command handlers, ISR deferral): use function pointers. They allocate nothing, use ¼ of the RAM per table entry, and invoke faster than delegates on develop (on 1.x, about the same as a static delegate).
- **Captured state** (closures, instance targets, multicast, events): use delegates. Function pointers can only target static methods from C#.
- **Long-lived static delegates** (`static readonly Action a = M;`): invocation is close to a function pointer; the main win is memory (no object).

## 6. Tests

| Where | 1.x | develop |
|---|---:|---:|
| MDP unit tests (`FunctionPointersTests.cs`) | 22 | 22 |
| CoreLibrary `NFUnitTestFunctionPointers` (on device or nanoCLR) | 35 | 51 (incl. `UnitTestGenericFunctionPointersTests.cs`) |
| CoreLibrary `NFUnitTestDelegates` (regression) | 46 | 46 |
| End-to-end harness (debug and stress-GC runtime) | 77 | 110 |
| Adversarial audit (forged pointers, mismatched signatures, cache poisoning, overload collisions) | 35 | 55 |
| Edge cases (40 mixed-type parameters, 20 struct parameters with a struct return, equality including generic instantiations, `void*` round-trip, recursion, a pointer called from another thread, `ref`/`out`/`in`, null call) | 13 | 17 |
| Reference-type variance (valid conversions plus forged boxing, by-ref, wrong-direction, value-type array and generic-argument cases) | 24 | 31 |
| Parity probe (generic call sites and targets, forged generic and open instantiations, nested and deep signatures, overrides, interfaces) | 17 (15 pass; the 2 failures are the pre-existing 1.x `ref readonly` return issue in §7) | 75 (68 pass; the 7 failures are the pre-existing generic-dispatch issues in §7, and they fail identically without function pointers) |

What the tests cover:

- **Signatures:** primitive types (`byte`, `short`, `int`, `long`, `double`, …), strings, objects, structs (large and generic), `ref`/`out`/`in`, `ref` returns, nested function pointers, 0–40 arguments, and overloads that differ only by function-pointer signature.
- **Dispatch:** virtual and interface overloads; arrays and statics of pointers; equality; delegate↔pointer round-trips; `sizeof`; exceptions thrown through `calli`; a pointer called from another thread.
- **Reference-type variance:** method-group and pointer-to-pointer conversions with contravariant parameters and covariant returns (classes, interfaces, arrays, generic instantiations, class-constrained `T`), and rejection of forged boxing, by-ref, wrong-direction, value-type array and generic-argument variance.
- **Validation cache:** one call site with many different targets; cache thrash (24 pairs for 16 entries); a pointer validated at one site and rejected at an incompatible site.
- **Develop:**
  - generic methods and types, open and closed;
  - instantiation identity across assemblies;
  - cctor triggering;
  - generic struct returns;
  - call sites that use the caller's `T` (`delegate*<T, T>`, `delegate*<G<T>, G<T>>`, `ref T`, `T[]`, nested `delegate*<delegate*<T, T>, T, T>`);
  - per-instantiation cache isolation;
  - forged pointers to closed and open instantiations, rejected;
  - 10-level nested generic signatures;
  - 7-level nested function-pointer signatures.

## 7. Known limitations and pre-existing issues

The following were found during testing. None is caused by this work, and apart from the first, none is fixed here:

- Develop: closed-generic resolution ignored array levels. **Fixed** in this work (§3.4).
- `Info.cpp` IL tracing only compiles with `VIRTUAL_DEVICE`.
- `ldvirtftn` on a generic base class fails. This affects delegates in the same way.
- Nested MVAR (`M<T>() => &N<List<T>>`-like nesting) asserts. Generic types that use only MVAR throw.
- Cross-assembly generic-struct `FieldRef` fails with "Unknown FieldRef".
- MDP: `RuntimeHelpersTests` triggers an assert. Building all of `NFUnitTestClasses` in one assembly overflows the string table.
- Develop: a virtual method overridden in a class derived from a closed generic base (`GDerived : GBase<int>`) dispatches to the base implementation. Calls on generic interfaces (`IGen<int>`) throw.
- Develop: a generic method called with another generic method's type parameter (`A<T>() => B<T>()`) runs with an open MethodSpec. Most type-dependent operations in `B` assert ("nested MVAR not implemented"). `calli` validation rejects such contexts (§4).
- Develop, stress-GC build only: an interface call on a boxed struct throws when the same method also passes a struct as an `in` argument.
- Develop: methods with nested generic struct locals (`S<S<S<int>>>`) do not run. Constructing nested generic class objects (`new C<C<C<int>>>`) throws.
- 1.x: a method that returns `ref readonly` (modreq `InAttribute` on the return type) corrupts the stack, even on a direct call. The 1.x MDP has no `RequiredModifierType` branch in `WriteDataType`, so it writes the return type as `void`. The PE is byte-identical with the upstream MDP. `ref readonly` *returns* through function pointers are therefore not usable on 1.x; `in` parameters work.
