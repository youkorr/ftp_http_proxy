import esphome.codegen as cg
import esphome.config_validation as cv

CONF_SERVER = 'server'
CONF_USERNAME = 'username'
CONF_PASSWORD = 'password'
CONF_REMOTE_PATHS = 'remote_paths'
CONF_LOCAL_PORT = 'local_port'

# Suppression des dépendances et auto_load liés au media_player
DEPENDENCIES = []
AUTO_LOAD = []

ftp_http_proxy_ns = cg.esphome_ns.namespace('ftp_http_proxy')
FTPHTTPProxy = ftp_http_proxy_ns.class_('FTPHTTPProxy', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.Required(CONF_SERVER): cv.string_,
    cv.Required(CONF_USERNAME): cv.string_,
    cv.Required(CONF_PASSWORD): cv.string_,
    cv.Required(CONF_REMOTE_PATHS): cv.All(
        cv.ensure_list,
        [cv.string_]
    ),
    cv.Optional(CONF_LOCAL_PORT, default=8000): cv.port,
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    # Configuration des paramètres
    cg.add(var.set_ftp_server(config[CONF_SERVER]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    
    # Ajout des chemins distants
    for remote_path in config[CONF_REMOTE_PATHS]:
        cg.add(var.add_remote_path(remote_path))
    
    cg.add(var.set_local_port(config[CONF_LOCAL_PORT]))
