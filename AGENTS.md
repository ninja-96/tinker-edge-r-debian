# General Rules

- No more than 2 agents may work concurrently.
- Write all user-facing responses, plans, and code reviews in Russian.
- Preserve code, identifiers, and commands in the project's language.
- Work autonomously within the task scope; avoid unnecessary questions.
- Prefer minimal changes without unnecessary refactoring.

# Tools

Use these tools proactively when relevant, without waiting for an explicit request.

- **Serena MCP** — primary tool for project memory and semantic code navigation.
  - Before using Serena, ensure the correct project is active; activate the current project root if needed and supported.
  - Read Serena's initial instructions once per session when available. Check onboarding status and complete onboarding if required.
  - Before starting work, check available memories and read those relevant to the task.
  - Prefer symbol-based tools for finding definitions and references, exploring code, and making suitable symbol-level edits.
  - Read only the symbols and file sections needed. Use text search or targeted file reads when symbol-based tools are unsuitable.
  - Store durable project knowledge: important decisions and their rationale, architecture, conventions, verified commands, and non-obvious details.
  - Update existing memories when information changes; avoid duplicates. Verify memories against current code when accuracy matters.
  - Follow the project's memory maintenance instructions when available.
  - Do not store temporary progress, routine task summaries, raw logs, secrets, or unverified assumptions.

- **Context7 MCP** — preferred tool for library and framework documentation.
  - Use it when implementation depends on external APIs, configuration options, setup steps, or library-specific behavior.
  - Identify the correct library and consult documentation matching the project's installed version when available.
  - Retrieve documentation relevant to the task rather than loading broad, unrelated material.
  - If the library, version, or required details are unavailable, use Hound to find official documentation.
  - Do not assume documentation for the latest version applies to the project.

- **Hound MCP** — primary tool for searching up-to-date information online.
  - Use it to verify current documentation, APIs, versions, compatibility, and unfamiliar or uncertain facts.
  - Prefer official documentation and primary sources.
  - Check that sources apply to the project's actual versions and environment.
  - Clearly distinguish verified facts from assumptions and inferences.

Use the terminal for commands, builds, and tests. If a tool is unavailable or fails, use a suitable alternative and briefly report any limitation that affects the result.

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
