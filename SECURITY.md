# Security

Hadal controls a PC, so security bugs matter more than anything else here. Thanks for looking.

## Reporting

Please don't open a public issue. Use **Security → Report a vulnerability** on this repo instead, so it stays private until there's a fix.

Include what you found, how to reproduce it, and what an attacker gets out of it. I'll answer as soon as I can and credit you in the release notes unless you'd rather not be named.

## Supported versions

Only the latest release gets fixes.

## In scope

- Anything reachable over the network on the PC (port 47810 on the Tailscale address), especially before the token check
- Pairing, the token, phone pinning and the lockout
- Getting the PC to do something outside the fixed action list
- File transfer escaping `Downloads\Hadal` or its `To phone` folder
- The named pipe between the service and the tray app
- How the app stores the token on the phone

## Out of scope

- Attacks that need code already running as your Windows user or as admin
- A compromised Tailscale account or a malicious device you added to your own tailnet
- Physical access to an unlocked phone with the app lock turned off
- What the paired phone can do through the touchpad, keyboard or stream. That is full access to your Windows session by design, the same as sitting at the PC
- Anti-cheat software or administrator windows blocking remote input (expected Windows behavior)
