# Glossary

Ubiquitous language for Beacon. Terms only — no implementation detail.

- **Provider**: a named agent ecosystem (claude, codex, pi) the hub integrates. Declares which capability planes it supports; the user toggles Usage and Buddy per provider.
- **Capability plane**: one of the three things a provider can feed: **Usage** (quota windows), **Sessions** (live session list + working/waiting/idle state), **Prompts** (remote approve/deny from the device). "Buddy" is the user-facing toggle covering Sessions + Prompts together.
- **Gating provider**: a provider whose agent asks the hub before executing a tool, and the hub's answer decides (claude, codex). The device's Approve/Deny actually resolves the agent's prompt.
- **Mirror provider**: a provider the hub can only observe, not answer (pi). The device shows session state and tap-to-open, but decisions happen where the agent itself asks.
- **Usage window**: a provider-defined quota period normalized by the hub to a percentage + reset epoch. Only real quota windows qualify — synthetic percentages (e.g. token cost with no cap) are not usage.
- **Authorizer**: an external resolver registered into pi-permission-system's ordered chain; it can allow, deny, or defer an "ask" decision. Beacon participates as one — it never replaces the policy brain.
- **Pass-through**: the outcome when Beacon has no human decision (hub gone, device dark, timeout, toggle off): the agent's own local flow proceeds as if Beacon were absent. Absence of a decision is never a deny.
