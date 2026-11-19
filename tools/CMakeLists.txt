# Command-line tools that work with real market data.

# One pass over a full ITCH day. It prints message counts and, per stock, the peak number
# of resting orders and the price range, which decide how the books are sized.
add_executable(litbook_itch_stats itch_stats.cpp)
target_link_libraries(litbook_itch_stats PRIVATE litbook::litbook litbook::warnings)

# Replays a full ITCH day through one book per stock and times every message. This is
# the headline benchmark (see the usage comment at the top of itch_replay.cpp).
add_executable(litbook_itch_replay itch_replay.cpp)
target_link_libraries(litbook_itch_replay PRIVATE litbook::litbook litbook::warnings)
