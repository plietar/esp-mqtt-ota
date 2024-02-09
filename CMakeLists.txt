idf_component_register(
  SRCS "mqtt_ota.c"
  INCLUDE_DIRS "include"
  REQUIRES mqtt
  PRIV_REQUIRES app_update)
