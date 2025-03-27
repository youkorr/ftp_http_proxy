import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import media_player

DEPENDENCIES = ['media_player']
AUTO_LOAD = ['media_player']

ftp_http_proxy_ns = cg.esphome_ns.namespace('ftp_http_proxy')
FTPHTTPProxy = ftp_http_proxy_ns.class_('FTPHTTPProxy', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.Required('server'): cv.string,
    cv.Required('username'): cv.string,
    cv.Required('password'): cv.string,
    cv.Required('remote_paths'): cv.All(
        cv.ensure_list,
        [cv.string]
    ),
    cv.Optional('local_port', default=8000): cv.port,
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    # Configuration des paramètres
    cg.add(var.set_ftp_server(config['server']))
    cg.add(var.set_username(config['username']))
    cg.add(var.set_password(config['password']))
    
    # Ajout des chemins distants
    for remote_path in config['remote_paths']:
        cg.add(var.add_remote_path(remote_path))
    
    cg.add(var.set_local_port(config['local_port']))
