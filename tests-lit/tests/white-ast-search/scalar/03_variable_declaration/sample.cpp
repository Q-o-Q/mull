/// TODO: IDE reporter reports location of mutation instruction which is '+'
/// but we would rather want to see the location of '5'.
int foo() {
  int var = 0;
  return var;
}

int main() {
  return foo() != 0;
}

/**
; RUN: cd / && %CLANG_EXEC -fembed-bitcode -g %s -o %s.exe
; RUN: cd %CURRENT_DIR
; RUN: sed -e "s:%PWD:%S:g" %S/compile_commands.json.template > %S/compile_commands.json
; RUN: (unset TERM; %MULL_EXEC -debug -enable-ast -test-framework CustomTest -mutators=scalar_value_mutator -reporters=IDE -ide-reporter-show-killed -compdb-path %S/compile_commands.json %s.exe 2>&1; test $? = 0) | %FILECHECK_EXEC %s --strict-whitespace --match-full-lines
; CHECK-NOT:{{^.*[Ee]rror.*$}}
; CHECK-NOT:{{^.*[Ww]arning.*$}}

; CHECK:[info] AST Search: looking for mutations in the source files (threads: 1)
; CHECK:[debug] AST Search: recording mutation "Scalar Value": {{.*}}sample.cpp:4:7

; CHECK:[info] Applying filter: AST mutation filter (threads: 1)
; CHECK:[debug] ASTMutationFilter: whitelisting mutation "Scalar Value": {{.*}}sample.cpp:4:7

; CHECK:[info] Applying filter: junk (threads: 1)
; CHECK:[debug] ASTMutationStorage: recording mutation "Scalar Value": {{.*}}sample.cpp:4:7

; CHECK:[info] Killed mutants (1/1):
; CHECK:{{^.*}}sample.cpp:4:7: warning: Killed: Replacing scalar with 0 or 42 [scalar_value_mutator]{{$}}
; CHECK:  int var = 0;
; CHECK:      ^
; CHECK:[info] Mutation score: 100%
; CHECK-EMPTY:
**/
