# Runbook: Ruby DSL to Emacs Lisp Transpiler

This runbook defines the procedural workflow for implementing, maintaining, and testing the Ruby DSL to Emacs Lisp transpiler.

## 1. Scope & Completion Criteria
- **Scope**: Development of the Ruby-to-Elisp transpiler (`~/codings/ruby-elisp-transpiler`), AST mapping via Prism, S-expression generation, and Emacs validation.
- **Completion Criteria**:
  1. Ruby DSL files can be converted into valid `.el` files.
  2. The generated `.el` byte-compiles cleanly under `emacs -Q --batch -f batch-byte-compile` with lexical binding.
  3. Transpiled code executes in Emacs and produces the expected runtime results.

## 2. Prerequisites & Environment
- **Ruby**: Ruby 3.3+ with `prism` standard gem.
- **Emacs**: GNU Emacs 28+ (current environment: Emacs 31.1).
- **Core Reference**: `~/.emacs.d/emacs-opcode-datasheet.md` for Emacs bytecode and VM conventions.
- **Parent Runbook**: `~/.emacs.d/runbook/ruby-elisp-transpiler/design-001.org`.

## 3. Inputs & Conventions
- **Source Language**: Ruby-like DSL files (suggested extension: `.rel` or `.rb`).
- **Target Language**: Emacs Lisp with `;; -*- lexical-binding: t; -*-`.
- **Target Directory**: `~/codings/ruby-elisp-transpiler/`.

## 4. Implementation Steps

### Step 1: Project Scaffolding
1. Create directory `~/codings/ruby-elisp-transpiler`.
2. Initialize `Gemfile` with `prism` and test dependencies.
3. Establish directory layout:
   ```text
   ruby-elisp-transpiler/
   ├── bin/
   │   └── ruby2elisp
   ├── lib/
   │   ├── ruby2elisp.rb
   │   └── ruby2elisp/
   │       ├── ast_visitor.rb
   │       ├── emitter.rb
   │       └── sexpr.rb
   ├── spec/ (or test/)
   └── examples/
   ```

### Step 2: AST Processing with Prism
1. Parse code with `Prism.parse(source)`.
2. Inherit from `Prism::Visitor` to handle AST nodes:
   - `DefNode`: Translate to `(defun name (args...) body...)`.
   - `CallNode`: Translate method invocations `obj.method(args)` or bare function calls `point()` to `(method obj args)`.
   - `LocalVariableWriteNode`: Translate to `(setq var val)` or `(let* ((var val)) ...)`.
   - `IfNode` / `UnlessNode`: Translate to `(if cond then else)`.
   - `BlockNode` / `LambdaNode`: Translate to `(lambda (...) body)`.

### Step 3: S-Expression Emitter
1. Build an in-memory S-expression representation or structured formatted string.
2. Ensure top-of-file contains:
   ```elisp
   ;;; output.el --- Transpiled from Ruby DSL -*- lexical-binding: t; -*-
   ```
3. Format S-expressions with clean indentation.

### Step 4: Emacs Verification Pipeline
1. Run automated test via bash:
   ```bash
   emacs -Q --batch -f batch-byte-compile output.el
   ```
2. Verify execution:
   ```bash
   emacs -Q --batch -l output.el --eval '(your-transpiled-function)'
   ```

## 5. Troubleshooting & Edge Cases
- **Lexical vs Dynamic Scoping**: Ensure lexical binding header is present in every emitted file. Special Emacs dynamic variables (e.g. `current-prefix-arg`) must be referenced as symbols without shadowing.
- **Naming Conventions**: Map Ruby's snake_case `foo_bar` to Elisp kebab-case `foo-bar`, or preserve configurable mapping.
- **Predicates**: Map Ruby methods ending in `?` (e.g., `nil?`, `empty?`) to Elisp predicates ending in `-p` or `p` (e.g., `null`, `string-empty-p`).
