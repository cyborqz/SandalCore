# Changes of tick-performance-1

@known-issues add
| **Server / Many players** | In a simulated load test with players that only walk in one place, one world server kept its 30 updates a second, each within 50 ms of its time, with a few dozen simulated players, and began to fall behind at about that size (updates then arrived up to 0.1 s late). Players who fight and ride cost more, and the server has not been measured with a real crowd. | Fewer players in one place. | Open: faster updates are planned. |

@roadmap-achieved add
- Server speed with many players: a fixed-rate server tick, the players' views of the world taking turns, cheaper updates and move answers at the original server's pace.
