---
title: SSH Access
---

# SSH Access

Starting with v1.2.0, SSH access is supported natively via the printer GUI.

Navigate to `Settings` > `Maintenance` > `Root Access` > Agree (scroll down to accept) > `Open`.

Toggle SSH from the printer's web interface:

1. Open `http://<printer-ip>/firmware-config/` in a browser.
2. Scroll down to the SSH access section and enable it.

> **LAN security**: with [LAN security](lan_security.md) enabled, SSH accepts public keys only and stays off until `extended/ssh/authorized_keys` has a key. The default password below no longer works.

## Credentials

- User: `root` / Password: `snapmaker`
- User: `lava` / Password: `snapmaker`

## Connecting

```bash
ssh root@<printer-ip>
```

Replace `<printer-ip>` with your printer's IP address.

## Changing Passwords

To persist password changes across reboots, enable data persistence. See [Data Persistence](data_persistence.md).

## Security Considerations

- SSH provides full system access to your printer
- Only enable SSH on trusted networks
- Consider disabling SSH when not actively needed
- Change default credentials after first login
