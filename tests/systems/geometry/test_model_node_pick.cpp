#include <corona/engine/engine_runtime_api.h>
#include <stdexcept>
int main() { Corona::API::ModelNodePickResult result{42,7}; if(result.actor_handle!=42 || result.node_index!=7) throw std::runtime_error("node pick identity"); }
