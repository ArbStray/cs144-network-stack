-- network.lua
--
-- Declarative description of the network_simulator topology. Loaded by
-- `apps/network_simulator` when invoked as `network_simulator <file>.lua`.
-- Run with no arguments to use the built-in hardcoded topology instead.
--
-- Schema:
--   interfaces = { {name=, ip=}, ... }            -- router interfaces
--   hosts      = { <name> = {ip=, next_hop=}, ... } -- end hosts
--   routes     = { {prefix=, len=, next_hop=, interface=}, ... }
--   connections = { {router=, host=} | {router=, hosts={...}}, ... }
--   tests      = { {from=, to=, desc=, [ttl=]}, ... }

return {
  interfaces = {
    { name = "default", ip = "171.67.76.46" },
    { name = "eth0",    ip = "10.0.0.1" },
    { name = "eth1",    ip = "172.16.0.1" },
    { name = "eth2",    ip = "192.168.0.1" },
    { name = "uun3",    ip = "198.178.229.1" },
    { name = "hs4",     ip = "143.195.0.2" },
    { name = "mit5",    ip = "128.30.76.255" },
  },

  hosts = {
    applesauce     = { ip = "10.0.0.2",       next_hop = "10.0.0.1" },
    default_router = { ip = "171.67.76.1",    next_hop = "0" },
    cherrypie      = { ip = "192.168.0.2",    next_hop = "192.168.0.1" },
    hs_router      = { ip = "143.195.0.1",    next_hop = "0" },
    dm42           = { ip = "198.178.229.42", next_hop = "198.178.229.1" },
    dm43           = { ip = "198.178.229.43", next_hop = "198.178.229.1" },
  },

  routes = {
    { prefix = "0.0.0.0",       len = 0,  next_hop = "default_router", interface = "default" },
    { prefix = "10.0.0.0",      len = 8,                              interface = "eth0" },
    { prefix = "172.16.0.0",    len = 16,                             interface = "eth1" },
    { prefix = "192.168.0.0",   len = 24,                             interface = "eth2" },
    { prefix = "198.178.229.0", len = 24,                             interface = "uun3" },
    { prefix = "143.195.0.0",   len = 17, next_hop = "hs_router",    interface = "hs4" },
    { prefix = "143.195.128.0", len = 18, next_hop = "hs_router",    interface = "hs4" },
    { prefix = "143.195.192.0", len = 19, next_hop = "hs_router",    interface = "hs4" },
    { prefix = "128.30.76.255", len = 16, next_hop = "128.30.0.1",   interface = "mit5" },
  },

  -- Physical (link-layer) connections between router interfaces and hosts.
  -- A connection may attach one host (`host = "name"`) or several hosts on
  -- the same segment (`hosts = {"a", "b"}`).
  connections = {
    { router = "default", host = "default_router" },
    { router = "eth0",    host = "applesauce" },
    { router = "eth2",    host = "cherrypie" },
    { router = "hs4",     host = "hs_router" },
    { router = "uun3",    hosts = { "dm42", "dm43" } },
  },

  -- Traffic tests to run after the topology is built.
  tests = {
    { from = "applesauce", to = "cherrypie",   desc = "ordinary hosts" },
    { from = "cherrypie",  to = "applesauce",  desc = "reverse direction" },
    { from = "applesauce", to = "1.2.3.4",      desc = "to the Internet" },
    { from = "applesauce", to = "143.195.131.17", desc = "HS network" },
    { from = "applesauce", to = "1.2.3.4",      desc = "TTL expiration", ttl = 1 },
  },
}
