---
name: ruby-elisp-transpiler
description: Use this skill when developing, enhancing, debugging, or verifying the Ruby DSL to Emacs Lisp transpiler, including Prism AST transformation, S-expression generation, and Emacs bytecode execution.
compatibility: Requires Ruby 3.3+, GNU Emacs 28+, bash, and git.
allowed-tools: bash edit view create grep glob ask_user
---

# Ruby DSL to Emacs Lisp Transpiler

Use this skill when authoring, modifying, or extending the Ruby DSL to Emacs Lisp
transpiler project at `~/codings/ruby-elisp-transpiler`.

## Resources

- Bytecode interchange schema: [`assets/emacs-bytecode.proto`](assets/emacs-bytecode.proto)
- Schema scope, validation contract, and receiver handoff: [`assets/handoff.org`](assets/handoff.org)
- Feature Lifecycle Runbook:
  [`~/.emacs.d/runbook/ruby-elisp-transpiler/design-001.org`](../../../runbook/ruby-elisp-transpiler/design-001.org)
- Procedural Runbook:
  [`../../runbooks/ruby-elisp-transpiler.md`](../../runbooks/ruby-elisp-transpiler.md)
- Emacs Opcode Datasheet:
  [`~/.emacs.d/emacs-opcode-datasheet.md`](../../../emacs-opcode-datasheet.md)

## Workflow

1. **Check Status**:
   - Inspect `~/.emacs.d/runbook/ruby-elisp-transpiler/design-001.org` to check current lifecycle state and active tasks.
2. **Implement & Enhance**:
   - Work in `~/codings/ruby-elisp-transpiler`.
   - Use `Prism` for AST parsing and `Prism::Visitor` for AST walking.
   - Maintain clean S-expression output with `lexical-binding: t`.
3. **Verify in Emacs**:
   - Always run batch compilation checks:
     `emacs -Q --batch -f batch-byte-compile <generated-file>.el`
   - Run integration tests comparing expected Ruby execution with Elisp batch eval.
4. **Update Lifecycle**:
   - Record progress, completed tasks, and newly discovered edge cases in `design-001.org`.

## Required Behavior

- Always ensure emitted `.el` files specify `lexical-binding: t`.
- Do not bypass Emacs batch compilation validation; code generation is only considered valid if it compiles cleanly without warnings.
- Keep the Ruby source syntax clean and idiomatic; do not force Lisp-isms into the user-facing Ruby DSL.
