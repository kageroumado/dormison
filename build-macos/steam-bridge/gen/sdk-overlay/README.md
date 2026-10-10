# SDK overlay

Headers the generator needs that no pinned Proton commit carries. `fetch-sdk.sh` lays
them over the Proton set.

## steamworks_sdk_153

Steamworks SDK 1.53 (the initial release, before 1.53a) is the one that defines
`SteamNetworkingSockets011`, which Steamworks.NET 20.x games ask for. Proton has 1.52
(`SteamNetworkingSockets009`) and 1.53a (`012`). These four networking headers are
Valve's own of that release, taken from the shared-source GameNetworkingSockets tree at
the commit whose `isteamnetworkingsockets.h` is blob `a7c49952b6a3fb2d79975b65cfe0fe6ea46f9b47`
(2021-08-01, `include/steam/`): `isteamnetworkingsockets.h`, `steamnetworkingtypes.h`,
`isteamnetworkingmessages.h`, `isteamnetworkingutils.h`. Everything else in the
synthesized directory is 1.53a's, minus `steamnetworkingfakeip.h`, whose struct still
lived in the types header in 1.53.
