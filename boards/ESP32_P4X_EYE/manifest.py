freeze("$(PORT_DIR)/modules")
include("$(MPY_DIR)/extmod/asyncio")
require("umqtt.simple")
require("umqtt.robust")
