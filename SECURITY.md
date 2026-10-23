# Security Policy

## Project Scope and Intended Use

ESP-VISION is a developer-focused edge AI and computer vision framework for learning, evaluation, prototyping, and personal DIY projects. The firmware, board configurations, and examples in this repository are development references, not a production SDK or a ready-to-deploy configuration for mass-produced customer products. Integrators must assess and implement the security controls required for their own deployments.

## Development and Debug Interfaces

The EV-MUX TCP control transport provides wireless development and debugging on a trusted local network. It exposes development operations such as the MicroPython REPL, file uploads, script execution, network configuration, and device reset.

The current TCP control transport does not provide peer authentication or transport encryption. The first accepted session acquisition request obtains an available session. Session ownership, client nonces, session IDs, VM epochs, and CRC checks support protocol coordination, request isolation, and error detection; they do not authenticate the operator or provide a security boundary against other reachable clients.

When a board enables `ESP_VISION_ENABLE_TCP_TRANSPORT` and its background network service is enabled and online, the listener opens on the station IPv4 address, using port `3232` by default. It may therefore become reachable automatically after provisioning or reconnection. Binding to that address does not restrict access to clients on the same subnet; reachability depends on the network configuration.

Use this interface only on an isolated development network whose participants are trusted to control the device. Do not expose it to the Internet, shared or untrusted networks, or port forwarding. Wi-Fi credentials control network access and do not authenticate individual EV-MUX clients. If wireless debugging is unnecessary, set `ESP_VISION_ENABLE_TCP_TRANSPORT` to `0` in the selected board's `boardconfig.h` and rebuild the firmware; serial development interfaces remain available as configured for the board.

## Bug Bounty Program

ESP-VISION is a developer-focused reference framework for evaluation, prototyping, and personal DIY projects, rather than a production SDK. As such, **ESP-VISION is not currently included in the Espressif Bug Bounty Program.** Reports concerning this repository are not eligible for bounty payments under that program. Any future inclusion will be announced explicitly in this policy.

We continue to welcome responsible security reports and use them to improve the project. If a report also identifies an issue in another Espressif product or production SDK, that issue must be evaluated separately under the [Espressif Bug Bounty Program](https://www.espressif.com/en/node/9599) scope and rules. This repository's exclusion does not determine the eligibility of other products or repositories.

## Reporting a Security Issue

Please report suspected security issues privately to [bugbounty@espressif.com](mailto:bugbounty@espressif.com), following the [Espressif Security Incident Response Process](https://www.espressif.com/en/node/9599). This reporting channel also accepts reports that are outside the bounty program.

Include the affected release or commit, board and firmware configuration, relevant network or physical access requirements, reproduction steps or a proof of concept, and the expected and observed impact.

Do not post sensitive vulnerability details or credentials in public GitHub issues. Coordinate disclosure with the security team while the report is assessed. For ordinary functional problems, use the [GitHub issue forms](https://github.com/espressif/esp-vision/issues/new/choose).
