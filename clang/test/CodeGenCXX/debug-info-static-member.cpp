// RUN: %clangxx -target x86_64-unknown-unknown -g %s -emit-llvm -S -o - | FileCheck --check-prefixes=CHECK,NOT-MS %s
// RUN: %clangxx -target x86_64-unknown-unknown -g -std=c++98 %s -emit-llvm -S -o - | FileCheck --check-prefixes=CHECK,NOT-MS %s
// RUN: %clangxx -target x86_64-unknown-unknown -g -std=c++11 %s -emit-llvm -S -o - | FileCheck --check-prefixes=CHECK,NOT-MS %s
// RUN: %clangxx -target x86_64-windows-msvc -g %s -emit-llvm -S -o - | FileCheck --check-prefixes=CHECK %s
// PR14471

// CHECK: @{{.*}}a{{.*}} = dso_local global i32 4, align 4, !dbg [[A:![0-9]+]]
// CHECK: @{{.*}}b{{.*}} = dso_local global i32 2, align 4, !dbg [[B:![0-9]+]]
// CHECK: @{{.*}}c{{.*}} = dso_local global i32 1, align 4, !dbg [[C:![0-9]+]]

enum X {
  Y
};
class C
{
  static int a;
  const static bool const_a = true;
protected:
  static int b;
#if __cplusplus >= 201103L
  constexpr static float const_b = 3.14;
#else
  const static float const_b = 3.14;
#endif
public:
  static int c;
  const static int const_c = 18;
  int d;
  static X x_a;
};

// The definition of C::a drives the emission of class C, which is
// why the definition of "a" comes before the declarations while
// "b" and "c" come after.

// CHECK: [[A]] = !DIGlobalVariableExpression(var: [[AV:.*]], expr: !DIExpression())
// CHECK: [[AV]] = distinct !DIGlobalVariable(name: "a",
// CHECK-SAME:                                declaration: ![[DECL_A:[0-9]+]])
//
// CHECK: !DICompositeType(tag: DW_TAG_enumeration_type, name: "X"{{.*}})
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "anon_static_decl_struct"
// CHECK: !DIDerivedType(tag: DW_TAG_member, name: "anon_static_decl_var"
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "static_decl_templ<int>"
// CHECK-NOT:              DIFlagFwdDecl
// CHECK-SAME:             ){{$}}
// CHECK: !DIDerivedType(tag: DW_TAG_member, name: "static_decl_templ_var"

int C::a = 4;
// CHECK: [[B]] = !DIGlobalVariableExpression(var: [[BV:.*]], expr: !DIExpression())
// CHECK: [[BV]] = distinct !DIGlobalVariable(name: "b",
// CHECK-SAME:                                declaration: ![[DECL_B:[0-9]+]])
// CHECK: ![[DECL_B]] = !DIDerivedType(tag: DW_TAG_member, name: "b"
// CHECK-NOT:                                 size:
// CHECK-NOT:                                 align:
// CHECK-NOT:                                 offset:
// CHECK-SAME:                                flags: DIFlagProtected | DIFlagStaticMember)
//
// CHECK: !DICompositeType(tag: DW_TAG_class_type, name: "C"{{.*}})
//
// CHECK: ![[DECL_A]] = !DIDerivedType(tag: DW_TAG_member, name: "a"
// CHECK-NOT:                                 size:
// CHECK-NOT:                                 align:
// CHECK-NOT:                                 offset:
// CHECK-SAME:                                flags: DIFlagStaticMember)
//
// CHECK:    ![[CONST_A_DECL:[0-9]+]] = !DIDerivedType(tag: DW_TAG_member, name: "const_a"
// CHECK-NOT:                           size:
// CHECK-NOT:                           align:
// CHECK-NOT:                           offset:
// CHECK-SAME:                          flags: DIFlagStaticMember
// CHECK-NOT:                           extraData:

// CHECK:     ![[CONST_B_DECL:[0-9]+]] = !DIDerivedType(tag: DW_TAG_member, name: "const_b"
// CHECK-NOT:                            size:
// CHECK-NOT:                            align:
// CHECK-NOT:                            offset:
// CHECK-SAME:                           flags: DIFlagProtected | DIFlagStaticMember
// CHECK-NOT:                            extraData:

// CHECK: ![[DECL_C:[0-9]+]] = !DIDerivedType(tag: DW_TAG_member, name: "c"
// CHECK-NOT:                                 size:
// CHECK-NOT:                                 align:
// CHECK-NOT:                                 offset:
// CHECK-SAME:                                flags: DIFlagPublic | DIFlagStaticMember)
//
// CHECK:     ![[CONST_C_DECL:[0-9]+]] = !DIDerivedType(tag: DW_TAG_member, name: "const_c"
// CHECK-NOT:                            size:
// CHECK-NOT:                            align:
// CHECK-NOT:                            offset:
// CHECK-SAME:                           flags: DIFlagPublic | DIFlagStaticMember
// CHECK-NOT:                            extraData:
//
// CHECK: !DIDerivedType(tag: DW_TAG_member, name: "x_a"
// CHECK-SAME:           flags: DIFlagPublic | DIFlagStaticMember)

int C::b = 2;
// CHECK: [[C]] = !DIGlobalVariableExpression(var: [[CV:.*]], expr: !DIExpression())
// CHECK: [[CV]] = distinct !DIGlobalVariable(name: "c", {{.*}} declaration: ![[DECL_C]])
int C::c = 1;

int main()
{
        C instance_C;
        instance_C.d = 8;
        return C::c;
}

// CHECK-NOT: !DIGlobalVariable(name: "anon_static_decl_var"

// Test this in an anonymous namespace to ensure the type is retained even when
// it doesn't get automatically retained by the string type reference machinery.
namespace {
struct anon_static_decl_struct {
  static const int anon_static_decl_var = 117;
};
}

int ref() {
  return anon_static_decl_struct::anon_static_decl_var;
}

template<typename T>
struct static_decl_templ {
  static const int static_decl_templ_var = 7;
};

template<typename T>
const int static_decl_templ<T>::static_decl_templ_var;

int static_decl_templ_ref() {
  return static_decl_templ<int>::static_decl_templ_var;
}

// Verify that even when a static member declaration is created lazily when
// creating the definition, the declaration line is that of the canonical
// declaration, not the definition. Also, since we look at the canonical
// definition, we should also correctly emit the constant value (42) into the
// debug info.
struct V {
  virtual ~V(); // cause the definition of 'V' to be omitted by no-standalone-debug optimization
  static const int const_va = 42;
};

// const_va is not emitted for MS targets.
// NOT-MS: !DIDerivedType(tag: DW_TAG_member, name: "const_va",
// NOT-MS-SAME:           line: [[@LINE-5]]
// NOT-MS-NOT:            extraData:
const int V::const_va;

namespace x {
struct y {
// CHECK: !DIGlobalVariable(name: "z",
// CHECK-SAME:              scope: [[NS_X:![0-9]+]]
// CHECK: [[NS_X]] = !DINamespace(name: "x"
  static int z;
};
int y::z;
}

// CHECK:      !DIGlobalVariableExpression(var: ![[CONST_A_VAR:[0-9]+]], expr: !DIExpression(DW_OP_constu, 1, DW_OP_stack_value))
// CHECK:      ![[CONST_A_VAR]] = distinct !DIGlobalVariable(name: "const_a"
// CHECK-SAME:                    isLocal: true, isDefinition: true, declaration: ![[CONST_A_DECL]])

// CHECK:      !DIGlobalVariableExpression(var: ![[CONST_B_VAR:[0-9]+]], expr: !DIExpression(DW_OP_constu, {{.*}}, DW_OP_stack_value))
// CHECK:      ![[CONST_B_VAR]] = distinct !DIGlobalVariable(name: "const_b"
// CHECK-SAME:                    isLocal: true, isDefinition: true, declaration: ![[CONST_B_DECL]])

// CHECK:      !DIGlobalVariableExpression(var: ![[CONST_C_VAR:[0-9]+]], expr: !DIExpression(DW_OP_constu, 18, DW_OP_stack_value))
// CHECK:      ![[CONST_C_VAR]] = distinct !DIGlobalVariable(name: "const_c"
// CHECK-SAME:                    isLocal: true, isDefinition: true, declaration: ![[CONST_C_DECL]])
