idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "." "../private_include" "../../stratum/test" "stubs"
                                    "../../../main/tasks" "../../../main/power" "../../../main/self_test" "../../../main"
                       REQUIRES cmock stratum stratum_v2 asic)
