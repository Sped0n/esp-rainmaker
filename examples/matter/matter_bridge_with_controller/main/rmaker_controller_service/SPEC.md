# Matter Controller Service

The Matter Controller Service offers an interface for managing the Matter-Controller RainMaker device after it has booted as a Matter bridge and joined a Matter fabric.

## 1. Service

| Type                          | Name          |
|-------------------------------|---------------|
| esp.service.matter-controller | **MatterCTL** |

## 2. Parameters

| Type                        | Name          | Value Type | Default | Flag  |
|-----------------------------|---------------|------------|---------|-------|
| esp.param.base-url          | BaseURL       | string     |         | R W P |
| esp.param.user-token        | UserToken     | string     |         | W P   |
| esp.param.rmaker-group-id   | RMakerGroupID | string     |         | R W P |
| esp.param.matter-node-id    | MatterNodeID  | string     |         | R     |
| esp.param.matter-ctl-cmd    | MTCtlCMD      | int        | -1      | W     |
| esp.param.matter-ctl-status | MTCtlStatus   | int        | 0       | R P   |

### 2.1 BaseURL Parameter

This parameter stores the Endpoint URL that is used for HTTP REST APIs.

### 2.2 UserToken Parameter

This parameter stores the refresh token. This parameter SHALL be used to fetched access token in `user/login2` HTTP REST API. For the HTTP REST Authenticated APIs, the access_token SHALL be passed in the "Authorization" HTTP header as the authentication token.

**Note**: The access token will be expired after 1 hour since it is fetched.

### 2.3 RMakerGroupID Parameter

This parameter reports the RainMaker Group Id that the controller auto-discovers for the local commissioned Matter fabric. In RainMaker Matter Fabric, each RainMaker group corresponds to a Matter Fabric.

`RMakerGroupID` remains writable in the service contract for compatibility, but the controller runtime currently ignores incoming writes and overwrites the value with the discovered group id during startup.

### 2.4 MatterNodeID Parameter

This parameter stores the Matter Node ID associated with the NOC that is issued for the Matter Controller. It is represented as an uppercase hexadecimal string.

### 2.5 MTCtlCMD Parameter

This parameter corresponds to the command that the cloud sends to the Matter Controller. Here defines two command enumerations.

| Command Code Value | Command Name     |
|--------------------|------------------|
| 1                  | UpdateNOC        |
| 2                  | UpdateDeviceList |

#### 2.5.1 UpdateNOC command

The UpdateNOC command allows the controller to fetch the Rainmaker Controller NOC after the controller is authorized. When receiving this command, the controller will generate a new CSR and send it to the cloud. After receiving the response, it will install the new NOC in the response.

#### 2.5.2 UpdateDeviceList command

This UpdateDeviceList command allows the controller to fetch the Matter devices in its RainMaker Group(Matter Fabric) after the controlller is authorized. This command SHALL be excuted when a Matter device is added/removed.

### 2.6 MTCtlStatus Parameter

This parameter is a bitmap value which corresponds to the status of Matter Controller

| Bit | Name               | Summary                                                              |
|-----|--------------------|----------------------------------------------------------------------|
| 0   | BaseURLSet         | Whether the BaseURL paramater is set                                 |
| 1   | UserTokenSet       | Whether the UserToken paramater is set                               |
| 2   | AccessTokenSet     | Whether the AccessToken is fetched                                   |
| 3   | RmakerGroupIDSet   | Whether the RMakerGroupID paramater is set                           |
| 4   | MatterFabricIDSet  | Whether the MatterFabricID corresponding to RMakerGroupID is fetched |
| 5   | MatterNodeIDSet    | Whether the controller's Matter NodeID is fetched                    |
| 6   | MatterNOCInstalled | Whether the controller's Matter NOC Chain is installed               |

## 3. Matter Controller Initialization

Here are the steps for Matter controller initialization.

- Device boots as a Matter bridge and creates an aggregator endpoint before `esp_matter::start(...)`.

- Matter controller gets network credentials with wifi-provisioning or other custom methods.

- User commissions the device into a Matter fabric as that bridge.

- Phone APP sends `setparams` command with the `--data` payload `{"MatterCTL":{"BaseURL": <base-url>, "UserToken": <refresh-token>}}`

- Controller service fetches an access token and inspects the local commissioned Matter fabric table.

- Controller service auto-discovers the `RMakerGroupID` that matches the commissioned bridge fabric.

- Phone APP receives report of `MTCtlStatus` with all the bits set to `true` before timeout.

- Phone APP receives report of the auto-discovered `RMakerGroupID`.

- Controller service issues and installs the controller NOC on that same commissioned bridge fabric.

- Phone App receives report of `MatterNodeID` and it will never establish CASE session with that node ID.

- Phone APP sends `setparams` command with the `--data` payload `{"MatterCTL":{"MTCtlCMD": 2}}` to make the controller obtain the device list in the Matter Fabric.

If no local commissioned fabric exists yet, auto-discovery stops until bridge commissioning completes.
