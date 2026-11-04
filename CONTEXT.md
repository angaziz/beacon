# Glossary

Ubiquitous language for Beacon. Terms only — no implementation detail.

- **Provider**: a named agent ecosystem (claude, codex) the hub integrates. Declares which capability planes it supports; the user toggles Usage and Buddy per provider.
- **Capability plane**: one of the three things a provider can feed: **Usage** (quota windows), **Sessions** (live session list + working/waiting/idle state), **Prompts** (remote approve/deny from the device). "Buddy" is the user-facing toggle covering Sessions + Prompts together.
- **Gating provider**: a provider whose agent asks the hub before executing a tool, and the hub's answer decides (claude). The device's Approve/Deny actually resolves the agent's prompt.
- **Usage window**: a provider-defined quota period normalized by the hub to a percentage + reset epoch. Only real quota windows qualify — synthetic percentages (e.g. token cost with no cap) are not usage.
- **Pass-through**: the outcome when Beacon has no human decision (hub gone, device dark, timeout, toggle off): the agent's own local flow proceeds as if Beacon were absent. Absence of a decision is never a deny.
