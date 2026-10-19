#include <corona/systems/geometry/model_node_state.h>
#include <stdexcept>
int main() { auto matrix=Corona::ModelNodes::trs({1,2,3},{0,0,0},{1,1,1}); if(matrix[12]!=1 || matrix[13]!=2 || matrix[14]!=3) throw std::runtime_error("trs translation"); }
