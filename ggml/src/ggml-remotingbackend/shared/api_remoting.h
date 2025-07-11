#define VIRGL_APIR_COMMAND_TYPE_LoadLibrary 331
#define VIRGL_APIR_COMMAND_TYPE_Forward 332

static inline const char *api_remoting_command_name(int32_t type)
{
  switch (type) {
  case VIRGL_APIR_COMMAND_TYPE_LoadLibrary: return "LoadLibrary";
  case VIRGL_APIR_COMMAND_TYPE_Forward: return "Forward";
  default: return "unknown";
  }
}
