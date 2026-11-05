# General Rules

- No more than 2 agents may work concurrently.
- Write all user-facing responses, plans, and code reviews in Russian.
- Preserve code, identifiers, and commands in the project's language.
- Work autonomously within the task scope; avoid unnecessary questions.
- Prefer minimal changes without unnecessary refactoring.

# Tools

- **Serena MCP** — primary long-term memory and code navigation tool.
  - Check relevant memories before starting work.
  - Prefer symbol-based search over reading entire files when possible.
  - Store important decisions, architecture, conventions, and non-obvious project details.
  - Update outdated memories; avoid duplicates and temporary noise.
- **Hound MCP** — tool for searching up-to-date information online.
  - Use it to verify documentation, APIs, versions, and unfamiliar facts.
  - Prefer official and primary sources.

# Development

- Follow the project's existing architecture and coding style.
- Examine related code and dependencies before making changes.
- Run relevant tests and checks after changes when possible.
- Avoid modifying unrelated code unless necessary.
- Do not create commits unless explicitly requested.

# Git & Releases

- Commit messages must be concise, informative, and consistent with Git history.
- Release notes must be detailed, structured, and consistent with previous releases.
- Never claim tests passed unless they were actually executed.
