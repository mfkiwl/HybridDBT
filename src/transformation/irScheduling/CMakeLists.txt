set(SOURCE
   ${SOURCE}
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_scoreboard_hw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_scoreboard_sw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_list_hw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_list_sw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling.cpp
   PARENT_SCOPE
)


set(SOURCE_DBT
   ${SOURCE_DBT}
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_scoreboard_sw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling_scoreboard_hw.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/irScheduling.cpp
   PARENT_SCOPE
)
