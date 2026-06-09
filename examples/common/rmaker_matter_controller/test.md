## RainMaker CLI Discovery

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli --help
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request --help
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getparams --help
```

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getuserinfo
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getnodes
```

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getnodestatus TLU57uLFowdGR9NZFLCvan
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getnodestatus Tn9fGuAgHvqQKCbESKztBK
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getparams TLU57uLFowdGR9NZFLCvan
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli getparams Tn9fGuAgHvqQKCbESKztBK
```

## Remote Control

Read `OnOff` attribute:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4354 '{"matter_node_id":"0x4eed093f65178034","attribute_paths":[{"endpoint_id":"0x1","cluster_id":"0x6","attribute_id":"0x0"}]}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```

Write `StartUpOnOff=0`:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4353 '{"objects":[{"matter_node_id":"0x4eed093f65178034","matter_endpoint_id":"0x1"}],"request_payload":{"cluster_id":"0x6","attribute_id":"0x4001","attribute_value":{"0:U8":0}}}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```

Invoke `Off` while light was already off:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4352 '{"objects":[{"matter_node_id":"0x4eed093f65178034","matter_endpoint_id":"0x1"}],"request_payload":{"cluster_id":"0x6","command_id":"0x00","command_fields":{}}}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```

Toggle once:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4352 '{"objects":[{"matter_node_id":"0x4eed093f65178034","matter_endpoint_id":"0x1"}],"request_payload":{"cluster_id":"0x6","command_id":"0x02","command_fields":{}}}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```

Read `OnOff` after toggle:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4354 '{"matter_node_id":"0x4eed093f65178034","attribute_paths":[{"endpoint_id":"0x1","cluster_id":"0x6","attribute_id":"0x0"}]}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```

## Multi-Round Remote Control Regression

Initial read of `OnOff` and `StartUpOnOff`:

```sh
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli create_cmd_request TLU57uLFowdGR9NZFLCvan 4354 '{"matter_node_id":"0x4eed093f65178034","attribute_paths":[{"endpoint_id":"0x1","cluster_id":"0x6","attribute_id":"0x0"},{"endpoint_id":"0x1","cluster_id":"0x6","attribute_id":"0x4001"}]}' --timeout 60
uvx --from="esp-rainmaker-cli@1.12.0" esp-rainmaker-cli get_cmd_requests AAAAAAAAAAAAAAAAAAAAAA
```
