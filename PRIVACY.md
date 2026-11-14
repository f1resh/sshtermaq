# SSHTermaq privacy policy

*Last updated: 23 October 2026*

SSHTermaq is an SSH, Telnet, Rlogin, Raw and serial client that runs on your own computer. It has no account, no
online service and no analytics.

## What SSHTermaq does not do
- It does not collect, send or sell any information about you or how you use it.
- It contains no telemetry, crash reporting, advertising or tracking.
- It does not contact any server of its developer, so the developer receives nothing from it.

On Windows the app's window shows its interface with Microsoft Edge WebView2, a component of Windows. Microsoft's
diagnostic-data settings in Windows apply to WebView2 as they do to Windows itself.

## What stays on your computer
SSHTermaq keeps these in its data folder (Windows: `%APPDATA%\SSHTermaq`; macOS:
`~/Library/Application Support/SSHTermaq`), and nowhere else:
- your saved sessions, folders and settings;
- trusted host keys;
- passwords you choose to save, encrypted for your user account (Windows DPAPI; on macOS with a key kept in your
  login Keychain);
- session logs, only when you turn logging on for a session;
- an application log of program events and of the connections you open (server address and port, start, end and errors), which contains no user names or passwords and is never sent anywhere.

Deleting the data folder (or uninstalling the app from the Microsoft Store, which removes its package data) removes all
of it.

## Connections you make
SSHTermaq connects only to the hosts, proxies, jump hosts and serial ports you configure, and sends them what an SSH,
Telnet, Rlogin, Raw or serial connection needs (for example your user name and the authentication you choose). What
those servers do with it is governed by their owners' policies.

Links in the app (such as the project page on GitHub) open in your web browser; GitHub's own privacy statement applies
there.

## Children
SSHTermaq is a technical tool and does not knowingly handle information about children.

## Changes and contact
Changes to this policy are published in this file, in the project's repository. Questions:
[open an issue](https://github.com/f1resh/sshtermaq/issues) on GitHub.
