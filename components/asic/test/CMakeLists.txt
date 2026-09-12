idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "." "../../stratum/test/fixtures"
                                    "../../../main/tasks" "../../../main/self_test"
                       REQUIRES cmock stratum stratum_v2 asic)
