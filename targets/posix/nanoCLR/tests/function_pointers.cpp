//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include "Core.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

static_assert(DATATYPE_FNPTR == 0x39, "Keep the compact function-pointer encoding in sync with the metadata processor.");

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckResult(HRESULT actual, HRESULT expected, const char *message)
{
    if (actual != expected)
    {
        std::fprintf(stderr, "HRESULT: actual 0x%08x, expected 0x%08x\n", (unsigned)actual, (unsigned)expected);
    }
    Check(actual == expected, message);
}

// Compact metadata fixtures deliberately bypass the external metadata processor.
struct Fixture
{
    struct Image
    {
        CLR_RECORD_ASSEMBLY header;
        CLR_RECORD_METHODDEF methods[8];
        CLR_RECORD_TYPEDEF types[4];
        CLR_UINT8 signatures[256];
        CLR_UINT8 code[512];
    } image{};

    static CLR_RT_Assembly &AllocateAssembly()
    {
        void *storage = std::calloc(1, sizeof(CLR_RT_Assembly));
        Check(storage != NULL, "allocate fixture assembly");
        return *static_cast<CLR_RT_Assembly *>(storage);
    }

    CLR_RT_Assembly &assembly = AllocateAssembly();
    CLR_RT_MethodDef_CrossReference methodOwners[8]{};
    CLR_RT_TypeDef_CrossReference typeInfo[4]{};
    CLR_RT_Thread *thread = NULL;
    CLR_RT_StackFrame *root = NULL;
    CLR_UINT16 signaturePosition = 0;
    CLR_UINT16 codePosition = 0;

    Fixture()
    {
        image.header.startOfTables[TBL_MethodDef] = offsetof(Image, methods);
        image.header.startOfTables[TBL_TypeDef] = offsetof(Image, types);
        image.header.startOfTables[TBL_Signatures] = offsetof(Image, signatures);
        image.header.startOfTables[TBL_Signatures + 1] = offsetof(Image, code);
        image.header.startOfTables[TBL_ByteCode] = offsetof(Image, code);
        assembly.m_header = &image.header;
        assembly.m_idx = 1;
        assembly.m_pTablesSize[TBL_MethodDef] = ARRAYSIZE(image.methods);
        assembly.m_pTablesSize[TBL_TypeDef] = ARRAYSIZE(image.types);
        assembly.m_pCrossReference_MethodDef = methodOwners;
        assembly.m_pCrossReference_TypeDef = typeInfo;
        g_CLR_RT_TypeSystem.m_assemblies[0] = &assembly;
        g_CLR_RT_TypeSystem.m_assembliesMax = 2;

        g_CLR_RT_WellKnownTypes.m_UInt32.Set(1, 0);
        g_CLR_RT_WellKnownTypes.m_Int32.Set(1, 1);
        g_CLR_RT_WellKnownTypes.m_String.Set(1, 2);
        image.types[0].dataType = DATATYPE_U4;
        image.types[1].dataType = DATATYPE_I4;
        image.types[2].dataType = DATATYPE_STRING;
        image.types[3].dataType = DATATYPE_CLASS;
        image.types[3].flags = CLR_RECORD_TYPEDEF::TD_Delegate;

        for (auto &method : image.methods)
        {
            method.flags = CLR_RECORD_METHODDEF::MD_Static;
            method.lengthEvalStack = 8;
            method.locals = CLR_EmptyIndex;
        }
    }

    ~Fixture()
    {
        if (thread != NULL)
        {
            thread->DestroyInstance();
        }
        g_CLR_RT_TypeSystem.m_assemblies[0] = NULL;
        std::free(&assembly);
    }

    CLR_UINT16 Signature(std::initializer_list<CLR_UINT8> bytes)
    {
        CLR_UINT16 offset = signaturePosition;
        for (auto byte : bytes)
        {
            image.signatures[signaturePosition++] = byte;
        }
        return offset;
    }

    void Method(int index, CLR_UINT16 signature, std::initializer_list<CLR_UINT8> code)
    {
        auto &method = image.methods[index];
        method.sig = signature;
        method.numArgs = image.signatures[signature + 1];
        method.retVal = image.signatures[signature + 2] != DATATYPE_VOID;
        method.RVA = codePosition;
        for (auto byte : code)
        {
            image.code[codePosition++] = byte;
        }
    }

    HRESULT Run(const std::vector<CLR_UINT8> &code)
    {
        Method(0, Signature({0, 0, DATATYPE_VOID}), {});
        for (auto byte : code)
        {
            image.code[codePosition++] = byte;
        }
        Check(SUCCEEDED(CLR_RT_Thread::CreateInstance(1, 0, thread, 0)), "create test thread");
        CLR_RT_MethodDef_Index index;
        index.Set(1, 0);
        CLR_RT_MethodDef_Instance method{};
        Check(method.InitializeFromIndex(index), "resolve root");
        Check(SUCCEEDED(CLR_RT_StackFrame::Push(thread, method, 0)), "push root");
        root = thread->CurrentFrame();
        return CLR_RT_Thread::Execute_IL(*root);
    }

    HRESULT Resume(const std::vector<CLR_UINT8> &code)
    {
        auto offset = codePosition;
        for (auto byte : code)
        {
            image.code[codePosition++] = byte;
        }
        root->m_IP = image.code + offset;
        return CLR_RT_Thread::Execute_IL(*root);
    }

    static void Calli(std::vector<CLR_UINT8> &code, CLR_UINT16 signature)
    {
        CLR_UINT32 token = CLR_TkFromType(TBL_Signatures, signature);
        code.push_back(0x29);
        for (int i = 0; i < 4; i++)
        {
            code.push_back((CLR_UINT8)(token >> (i * 8)));
        }
    }

    static void Stop(std::vector<CLR_UINT8> &code)
    {
        // Stop before popping the root so its result remains available for assertions.
        code.push_back(0xFE);
        code.push_back(0x00);
    }
};

void TestSignatureParsing()
{
    Fixture fixture;
    auto signature = fixture.Signature({DATATYPE_FNPTR, 0, 1, DATATYPE_VOID, DATATYPE_STRING, DATATYPE_I4});
    CLR_RT_SignatureParser parser{};
    parser.Initialize_TypeSpec(&fixture.assembly, fixture.assembly.GetSignature(signature));
    CLR_RT_SignatureParser::Element element;
    Check(SUCCEEDED(parser.Advance(element)), "parse managed pointer");
    Check(element.m_dt == DATATYPE_FNPTR, "preserve pointer signature type");
    Check(parser.m_sig == fixture.image.signatures + signature + 5, "consume the entire pointer signature");

    CLR_RT_HeapBlock field;
    parser.Initialize_TypeSpec(&fixture.assembly, fixture.assembly.GetSignature(signature));
    Check(SUCCEEDED(g_CLR_RT_ExecutionEngine.InitializeReference(field, parser)), "initialize pointer field");
    Check(field.DataType() == DATATYPE_U4 && field.IsZero(), "pointer fields default to null method index");

    CLR_RECORD_METHODDEF method{};
    method.locals = signature;
    method.numLocals = 2;
    CLR_RT_HeapBlock locals[2];
    Check(SUCCEEDED(g_CLR_RT_ExecutionEngine.InitializeLocals(locals, &fixture.assembly, &method)), "pointer locals");
    Check(locals[0].DataType() == DATATYPE_U4 && locals[0].IsZero(), "pointer local defaults to null");
    Check(locals[1].DataType() == DATATYPE_I4, "local after pointer is parsed correctly");

    auto unmanaged = fixture.Signature({DATATYPE_FNPTR, 1, 0, DATATYPE_VOID});
    parser.Initialize_TypeSpec(&fixture.assembly, fixture.assembly.GetSignature(unmanaged));
    Check(parser.Advance(element) == CLR_E_NOT_SUPPORTED, "reject unmanaged pointer signature");
}

void TestNestedSignatures()
{
    Fixture fixture;
    auto first = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_FNPTR, 0, 0, DATATYPE_I4});
    auto same = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_FNPTR, 0, 0, DATATYPE_I4});
    auto different = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_FNPTR, 0, 0, DATATYPE_U4});
    CLR_RT_SignatureParser left{};
    CLR_RT_SignatureParser right{};
    left.Initialize_MethodSignature(&fixture.assembly, fixture.assembly.GetSignature(first));
    right.Initialize_MethodSignature(&fixture.assembly, fixture.assembly.GetSignature(same));
    Check(CLR_RT_TypeSystem::MatchSignature(left, right), "match identical nested signatures");
    left.Initialize_MethodSignature(&fixture.assembly, fixture.assembly.GetSignature(first));
    right.Initialize_MethodSignature(&fixture.assembly, fixture.assembly.GetSignature(different));
    Check(!CLR_RT_TypeSystem::MatchSignature(left, right), "distinguish nested return types");
}

void TestManagedCall()
{
    Fixture fixture;
    auto signature = fixture.Signature({0, 2, DATATYPE_I4, DATATYPE_I4, DATATYPE_I4});
    fixture.Method(1, signature, {0x02, 0x03, 0x59, 0x2A}); // return arg0 - arg1
    auto locals = fixture.Signature({DATATYPE_FNPTR, 0, 2, DATATYPE_I4, DATATYPE_I4, DATATYPE_I4});
    fixture.image.methods[0].locals = locals;
    fixture.image.methods[0].numLocals = 1;
    // ldftn target; stloc.0; ldc.i4.s 42; ldc.i4.5; ldloc.0; calli
    std::vector<CLR_UINT8> code = {0xFE, 0x06, 1, 0, 0x0A, 0x1F, 42, 0x1B, 0x06};
    Fixture::Calli(code, signature);
    Fixture::Stop(code);
    CheckResult(fixture.Run(code), CLR_E_UNSUPPORTED_INSTRUCTION, "managed call returns to caller");
    Check(fixture.root->m_evalStackPos == fixture.root->m_evalStack + 1, "calli consumes pointer and arguments");
    Check(fixture.root->m_evalStack[0].NumericByRef().s4 == 37, "arguments and return value are preserved");
}

void TestStringVoidCall()
{
    Fixture fixture;
    auto signature = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_STRING});
    fixture.Method(1, signature, {0x02, 0x26, 0x2A});
    // The string object is rooted on the root frame before entering calli.
    std::vector<CLR_UINT8> code = {0xFE, 0x00};
    Check(fixture.Run(code) == CLR_E_UNSUPPORTED_INSTRUCTION, "prepare string caller");
    CLR_RT_HeapBlock string;
    Check(SUCCEEDED(CLR_RT_HeapBlock_String::CreateInstance(string, "hello!")), "create string argument");
    fixture.root->m_evalStack[0].Assign(string);
    fixture.root->m_evalStackPos = fixture.root->m_evalStack + 1;
    code = {0xFE, 0x06, 1, 0};
    Fixture::Calli(code, signature);
    Fixture::Stop(code);
    CheckResult(fixture.Resume(code), CLR_E_UNSUPPORTED_INSTRUCTION, "string-to-void indirect call");
    Check(fixture.root->m_evalStackPos == fixture.root->m_evalStack, "void call leaves no result");
}

void TestRejectedCalls()
{
    for (int test = 0; test < 14; test++)
    {
        Fixture fixture;
        auto signature = fixture.Signature({0, 0, DATATYPE_VOID});
        fixture.Method(1, signature, {0x2A});
        std::vector<CLR_UINT8> code = {0xFE, 0x06, 1, 0};
        HRESULT expected = CLR_E_WRONG_TYPE;
        switch (test)
        {
            case 0:
                code = {0x16}; // null
                expected = CLR_E_NULL_REFERENCE;
                break;
            case 1:
                code = {0x20, 0xFF, 0xFF, 0xFF, 0xFF}; // invalid assembly
                break;
            case 2:
                code = {0x20, 0xFF, 0xFF, 1, 0}; // invalid method
                break;
            case 3:
                code = {0x20, 0, 0, 2, 0}; // unloaded assembly
                break;
            case 4:
                fixture.image.signatures[signature] = PIMAGE_CEE_CS_CALLCONV_UNMGD;
                expected = CLR_E_NOT_SUPPORTED;
                break;
            case 5:
                fixture.image.signatures[signature] = PIMAGE_CEE_CS_CALLCONV_HASTHIS;
                expected = CLR_E_NOT_SUPPORTED;
                break;
            case 6:
                fixture.image.methods[1].flags = 0; // instance method
                break;
            case 7:
                signature = fixture.Signature({0, 0, DATATYPE_I4}); // wrong return type
                break;
            case 8:
                code.clear(); // missing pointer
                expected = CLR_E_STACK_UNDERFLOW;
                break;
            case 9:
                code = {0x14}; // object, not a method index
                break;
            case 10:
                signature = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_I4});
                code.insert(code.begin(), 0x16); // enough stack, wrong parameter count
                break;
            case 11:
                signature = sizeof(fixture.image.signatures) - 1; // truncated signature header
                break;
            case 12:
                fixture.image.signatures[signature] = PIMAGE_CEE_CS_CALLCONV_VARARG;
                expected = CLR_E_NOT_SUPPORTED;
                break;
            case 13:
                fixture.image.signatures[signature] = PIMAGE_CEE_CS_CALLCONV_GENERIC;
                expected = CLR_E_NOT_SUPPORTED;
                break;
        }
        Fixture::Calli(code, signature);
        Fixture::Stop(code);
        CheckResult(fixture.Run(code), expected, "reject invalid or unmanaged calli");
    }
}

void TestDelegateConstruction(bool instance)
{
    Fixture fixture;
    auto target = fixture.Signature({0, 0, DATATYPE_I4});
    fixture.Method(1, target, {0x1F, 42, 0x2A});
    if (instance)
    {
        fixture.image.methods[1].sig = fixture.Signature({PIMAGE_CEE_CS_CALLCONV_HASTHIS, 0, DATATYPE_I4});
        fixture.image.methods[1].numArgs = 1;
        fixture.image.methods[1].flags = CLR_RECORD_METHODDEF::MD_Virtual;
        fixture.methodOwners[1].m_data = 2;
    }
    fixture.Method(
        2,
        fixture.Signature({PIMAGE_CEE_CS_CALLCONV_HASTHIS, 2, DATATYPE_VOID, DATATYPE_OBJECT, DATATYPE_I4}),
        {});
    fixture.image.methods[2].numArgs = 3;
    fixture.image.methods[2].flags = CLR_RECORD_METHODDEF::MD_Constructor;
    fixture.methodOwners[2].m_data = 3;
    std::vector<CLR_UINT8> code;
    Fixture::Stop(code);
    CheckResult(fixture.Run(code), CLR_E_UNSUPPORTED_INSTRUCTION, "prepare delegate caller");
    if (instance)
    {
        Check(
            SUCCEEDED(CLR_RT_HeapBlock_String::CreateInstance(fixture.root->m_evalStack[0], "target")),
            "create delegate target");
        code = {0x25, 0xFE, 0x07, 1, 0, 0x73, 2, 0}; // dup; ldvirtftn; newobj
    }
    else
    {
        fixture.root->m_evalStack[0].SetObjectReference(NULL);
        code = {0xFE, 0x06, 1, 0, 0x73, 2, 0}; // ldftn; newobj
    }
    fixture.root->m_evalStackPos = fixture.root->m_evalStack + 1;
    Fixture::Stop(code);
    CheckResult(fixture.Resume(code), CLR_E_UNSUPPORTED_INSTRUCTION, "construct existing delegate from method index");
    auto delegate = fixture.root->m_evalStack[0].DereferenceDelegate();
    Check(delegate != NULL && delegate->DataType() == DATATYPE_DELEGATE_HEAD, "delegate was allocated");
    Check(delegate->DelegateFtn().m_data == 0x00010001, "delegate retains target method");
    Check(delegate->m_object.IsZero() != instance, "delegate retains instance only for instance methods");

    fixture.Method(3, fixture.Signature({PIMAGE_CEE_CS_CALLCONV_HASTHIS, 0, DATATYPE_I4}), {});
    fixture.image.methods[3].numArgs = 1;
    fixture.image.methods[3].flags = CLR_RECORD_METHODDEF::MD_DelegateInvoke;
    fixture.methodOwners[3].m_data = 3;
    code = {0x6F, 3, 0}; // callvirt Invoke
    Fixture::Stop(code);
    CheckResult(fixture.Resume(code), CLR_E_UNSUPPORTED_INSTRUCTION, "invoke existing delegate");
    Check(fixture.root->m_evalStack[0].NumericByRef().s4 == 42, "delegate call returns original result");
}

void TestPointerIdentityAndZeroArgumentCall()
{
    Fixture fixture;
    auto signature = fixture.Signature({0, 0, DATATYPE_I4});
    fixture.Method(1, signature, {0x1F, 42, 0x2A});
    // Two independent ldftn instructions for the same method must compare equal.
    std::vector<CLR_UINT8> code = {0xFE, 0x06, 1, 0, 0xFE, 0x06, 1, 0, 0xFE, 0x01, 0xFE, 0x06, 1, 0};
    Fixture::Calli(code, signature);
    Fixture::Stop(code);
    CheckResult(fixture.Run(code), CLR_E_UNSUPPORTED_INSTRUCTION, "zero-argument managed call");
    Check(fixture.root->m_evalStackPos == fixture.root->m_evalStack + 2, "preserve lower evaluation stack");
    Check(fixture.root->m_evalStack[0].NumericByRef().s4 == 1, "method pointer identity");
    Check(fixture.root->m_evalStack[1].NumericByRef().s4 == 42, "zero-argument return value");
}

void TestByReferenceCall()
{
    Fixture fixture;
    auto signature = fixture.Signature({0, 1, DATATYPE_VOID, DATATYPE_BYREF, DATATYPE_I4});
    fixture.Method(1, signature, {0x02, 0x1F, 42, 0x54, 0x2A}); // *arg0 = 42
    fixture.image.methods[0].locals = fixture.Signature({DATATYPE_I4});
    fixture.image.methods[0].numLocals = 1;
    std::vector<CLR_UINT8> code = {0x12, 0, 0xFE, 0x06, 1, 0}; // ldloca.s 0; ldftn
    Fixture::Calli(code, signature);
    Fixture::Stop(code);
    CheckResult(fixture.Run(code), CLR_E_UNSUPPORTED_INSTRUCTION, "managed call with byref parameter");
    Check(fixture.root->m_locals[0].NumericByRef().s4 == 42, "byref writes reach the caller");
}
} // namespace

int main()
{
    nanoHAL_Initialize();
    CLR_RT_Memory::Reset();
    Check(SUCCEEDED(CLR_RT_ExecutionEngine::CreateInstance()), "initialize CLR");
    TestSignatureParsing();
    TestNestedSignatures();
    TestManagedCall();
    TestStringVoidCall();
    TestRejectedCalls();
    TestDelegateConstruction(false);
    TestDelegateConstruction(true);
    TestPointerIdentityAndZeroArgumentCall();
    TestByReferenceCall();
    std::puts("Managed function pointer regression tests passed.");
    return 0;
}
