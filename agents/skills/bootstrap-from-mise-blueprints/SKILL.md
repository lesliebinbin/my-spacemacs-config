---
name: bootstrap-from-mise-blueprints
description: Use this skill when the user asks to initialize, scaffold, generate, or bootstrap a project from lesliebinbin/mise-blueprints. It discovers the published blueprint branch, confirms the destination and Copier input mode, protects non-empty directories, constructs or runs the Copier command, and verifies the generated project.
compatibility: Requires Git, network access to GitHub, and Copier. The selected blueprint may require additional tools after generation.
allowed-tools: bash glob rg view apply_patch ask_user
---

# Bootstrap from Mise Blueprints

Use this skill when a project should be generated from:

```text
https://github.com/lesliebinbin/mise-blueprints
```

The repository is an index of versioned Copier templates. Its default branch
is documentation, not a project template, so every generation command must
select a published blueprint branch explicitly.

## Inputs

Resolve these values before running Copier:

| Input | Example |
|---|---|
| Blueprint ref | `uv-python/v0.0.1` |
| Destination | `./triton-learning01` |
| Input mode | defaults, interactive, or data file |
| Answers file | `answers.yml`, when using data-file mode |

Treat phrases such as "this folder" carefully. Distinguish between generating
the project as the current directory and creating a named child directory.
When the destination is not unambiguous from the current working directory and
the user's wording, ask for the exact parent directory.

## Workflow

1. Inspect the current directory and repository status without modifying them.
2. Read the blueprint repository README to learn its current usage and
   published branch names. Prefer the repository's authoritative content over
   remembered versions.
3. If necessary, list remote template branches:

   ```bash
   git ls-remote --heads \
     https://github.com/lesliebinbin/mise-blueprints.git
   ```

4. Confirm the blueprint ref when more than one template is plausible. Never
   use the default branch as a template merely because no ref was supplied.
5. Resolve the destination to an explicit path and inspect it:
   - If it does not exist, Copier may create it.
   - If it exists and is empty, it may be used.
   - If it is non-empty, do not run Copier until the user explicitly confirms
     how to proceed. Do not delete or clear it.
6. Check that Copier is available:

   ```bash
   copier --version
   ```

   If it is missing, report that fact and ask before installing anything.
7. Construct the command for the selected input mode. If the user asks only
   what the command would be, show it without executing it.
8. When execution is requested, run Copier from the intended parent directory
   and then inspect the generated files.
9. Follow any generated project instructions only when they are relevant to
   the user's task. Do not automatically install dependencies or run
   project-specific setup merely because generation succeeded.

## Copier commands

### Generate with template defaults

```bash
copier copy --defaults \
  --vcs-ref <blueprint-ref> \
  https://github.com/lesliebinbin/mise-blueprints.git \
  <destination>
```

Example:

```bash
copier copy --defaults \
  --vcs-ref uv-python/v0.0.1 \
  https://github.com/lesliebinbin/mise-blueprints.git \
  ./triton-learning01
```

### Generate interactively

```bash
copier copy \
  --vcs-ref <blueprint-ref> \
  https://github.com/lesliebinbin/mise-blueprints.git \
  <destination>
```

Use interactive mode only when the execution environment supports its prompts.
Otherwise collect the answers first and use a data file.

### Generate with an answers file

```bash
copier copy --data-file <answers-file> \
  --vcs-ref <blueprint-ref> \
  https://github.com/lesliebinbin/mise-blueprints.git \
  <destination>
```

Inspect the selected branch's Copier questions before creating an answers
file. Do not invent variable names based on another blueprint.

## Required behavior

- Always pass `--vcs-ref` with a confirmed, published template branch.
- Use `copier copy`; do not assume that the repository name implies a `mise`
  scaffolding command.
- Do not overwrite, clear, or recursively delete a non-empty destination.
- Do not silently choose between a named child directory and the current
  directory.
- Do not execute when the user requested only a command or dry-run
  explanation.
- Do not assume cached branch names are current; consult the repository when
  the available templates or versions matter.
- Preserve the generated Copier answers file so future template updates remain
  possible when the template emits one.
- Surface Copier failures directly rather than presenting a partially
  generated directory as successful.

## Verification

After generation:

1. Confirm the destination exists and contains generated project files.
2. Inspect the generated Copier answers file, when present, and confirm it
   records the intended template source and ref.
3. Check repository status from the correct parent repository so generated
   files are not accidentally attributed to another worktree or submodule.
4. Read the generated README and tool-version configuration before running
   dependency installation, builds, or tests.

Generation is complete when Copier exits successfully, the intended
destination contains the selected blueprint output, and no unrelated files
were changed.
