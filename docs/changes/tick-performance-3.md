# Changes of tick-performance-3

@known-issues set Server / Many players
| **Server / Many players** | In a load test with simulated players that only walk in one place, the server keeps its 30 updates a second with about twice as many players as before the speed work, and then begins to fall behind (updates arrive late and play gets laggy). The simulated players are not real players: players who fight and ride cost more, and the server has not been measured with a real crowd. | Fewer players in one place. | Open: more speed work is planned. |

@roadmap-achieved set Server speed with many players
- Server speed with many players: a fixed-rate server tick, the players' views of the world taking turns, cheaper updates, one update built for all the players who see the same mob in the same state, and move answers at the original server's pace. In a load test with simulated players that walk, the server held its 30 updates a second with about twice as many of them as before.
