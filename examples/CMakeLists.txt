set(examples simple_download.cpp stream_audio.cpp)

foreach(example ${examples})
  get_filename_component(example_name ${example} NAME_WE)
  add_executable(${example_name} ${example})
  target_link_libraries(${example_name} PRIVATE yt-dlpp-lib fmt::fmt
                                                spdlog::spdlog)
endforeach()
