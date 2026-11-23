# General
- Respond in Russian; preserve the project's language in code, identifiers, and commands
- Work autonomously within scope. Ask only when necessary
- Use no more than 2 concurrent agents
- Make minimal, focused changes following existing architecture and style

# Tools
Use relevant MCP tools first, without waiting for a request. Use built-in tools when MCP tools are unavailable, fail, or do not support the operation. Use the terminal for builds, tests, and commands.
- Serena: primary project memory and code navigation tool
  - Ensure the correct project is active; read initial instructions and check onboarding once per session
  - Check relevant memories before work. Prefer symbol-based navigation and edits; read only necessary code
  - Maintain durable decisions, architecture, conventions, and verified project details. Update existing memories; avoid duplicates, temporary noise, and secrets
- Context7: first choice for library/framework documentation and API usage. Match the project's versions; use Hound if coverage is insufficient
- Hound: search for current information, compatibility, troubleshooting, and uncertain facts. Prefer official and primary sources
Reuse relevant results already in context. Report material tool failures; avoid repeated identical failing calls. Distinguish verified facts from assumptions

# Development & Git
- Examine related code and dependencies before editing; avoid unrelated changes
- Run relevant checks when possible. Never claim checks passed unless executed
- Do not commit unless requested. Match Git history for commit messages and previous releases for release notes