# Changelog

## 2.0.0

### Features
- Add support for device_type_list on every endpoint
- Add Matter command-response support for remote invoke, write, and read commands.
- Add Matter attribute report publishing through the `MTDevices` setup-service parameter.
- Add local Matter attribute report callback registration for applications.
- Add Matter device-list helper APIs for update, copy, destroy, print, and readiness checks.
- Add RainMaker publish batching and token-bucket rate limiting for Matter attribute reports.

### Changed
- Device-list update callbacks now receive a temporary read-only device list. Applications must copy it if they need to retain it.
- Matter controller no longer stores a shared device-list copy internally; ownership is moved to applications that need a cached list.
- Internal command-response and attribute-report enable hooks were renamed without the redundant `controller` infix.
- Attribute-report coalesce timing is now an internal constant instead of a Kconfig option.

## 1.0.1 Jun 5

### Bug fix
- Set payload_is_json for PUT/POST calling of app_rmaker_user_api

## 1.0.0

- First version of the ESP RainMaker Matter Controller component

### Added
- RainMaker Matter Controller Setup service
