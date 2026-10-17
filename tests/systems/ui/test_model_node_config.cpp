#include "cef/model_node_config.h"
#include <stdexcept>
int main() { using namespace Corona::Systems::UI::ModelNodeConfig; auto list=normalize(json::array({{{"node_index",2},{"node_path","root/a"},{"visible",false}}})); if(list.size()!=1 || !list[0].at("visible").is_boolean()) throw std::runtime_error("normalize"); }
