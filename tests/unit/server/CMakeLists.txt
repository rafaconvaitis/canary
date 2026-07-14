target_sources(
    canary_ut
    PRIVATE network/message/networkmessage_test.cpp
            network/protocol/protocolunity_test.cpp
            network/protocol/protocolunity_contract_test.cpp
            network/protocol/multiprotocol_test.cpp
)
