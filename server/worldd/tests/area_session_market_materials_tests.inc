// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_materials_tests.inc: what a buyer and a seller of the Marketplace are told of the stacks that come to their bags, through real sessions
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//    the recording   retail answers every buy and every removal with ONE ClientRecieveBatchedStorageEvents call before the answer: one Add of the units that came, with the "Acquired"
//                    pop-up as its callout; a stack that joins one the player holds arrives as one Add of the units that joined it, in the held stack's slot and under its guid.  The
//                    tests read 8 recorded events (5 purchases, 4 of them of a material, and 3 removals) and prove that the pop-up the server writes is the recorded one, field for field
//    the buy         the owner's playtest: a material bought onto a stack the buyer holds showed nothing in the game, because the event had no pop-up and was a ClientReceiveStorageEvent
//                    call after the answer.  Now a buy and a removal of a material, onto a held stack, into an empty slot, in part and over the end of a stack, send retail's call in
//                    retail's order, and what the client would hold after the events (by the rule of its own apply) is what the server holds
// ============================================================================
#include "items/acquired.h"
#include "market/market.h"

namespace market_material_test {

/// A recorded bag event of retail: a purchase of 1 Tin.
constexpr Recorded kRetailBuyTin{"1eb9c133e136000000000000000000000000000000000000000000c0210400d9130b790a95800000000000000000000000000000c01a73e28b5ad099a60e7e8baf0717851c44d9e5b9cf9837020000000000000000000000000000008035e6c417b5a0334d1dfc165f0f2e0a3988b2cb739f316f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f0000006019000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c6462647a646262647e6662626a6464604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c407648b2acdad8be7c507000000000000000000000000c080040000a07119000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c6462647a646262647e6662626a6464604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c407648b2acdad8be7c50700000000000000000000000000200400d9130b790a95800000000000000000000000000000c01a73e28b5ad099a60e7e8baf0717851c44d9e5b9cf9837020000000000000000000000000000008035e6c417b5a0334d1dfc165f0f2e0a3988b2cb739f316f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c07dd67cc1b01606f04540a6b093abc801455d6901f500d0000000000000000000000200000008005270000000000e0040000000c7ec63a0000000000000020000000608d39f1452de84c5307bfc5d7838b42ce01", 7093};
/// A recorded bag event of retail: a purchase of 1 Oak Wood, the first of two in one session.
constexpr Recorded kRetailBuyOakFirst{"9eb9c133e336000000000000000000000000000000000000000000c021af151613d218008080000000000000000000000000000080b6a4961b49113d92042bb2add4512518809854dc10993702000000000000000000000000000000006d492d3792227a240956645ba9a34a300031a9b821326f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f0000008019000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c64626c626c7a6462647864604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0de4eaed8dac8be7c507000000000000000000000000c080040000a09119000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c64626c626c7a6462647864604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0de4eaed8dac8be7c5070000000000000000000000000020af151613d218008080000000000000000000000000000080b6a4961b49113d92042bb2add4512518809854dc10993702000000000000000000000000000000006d492d3792227a240956645ba9a34a300031a9b821326f0400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000009408c86489cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e000000a000000020000000405b52cb8da4881e498215d956eaa812cc01", 7109};
/// A recorded bag event of retail: a purchase of 1 Oak Wood, the second of that session: it tops up the stack of the first.
constexpr Recorded kRetailBuyOakSecond{"9eb9c133e336000000000000000000000000000000000000000000c021af151613d218008080000000000000000000000000000080b6a4961b49113d92042bb2add451251800cbfeb412993702000000000000000000000000000000006d492d3792227a240956645ba9a34a300096fd6925326f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f0000008019000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c64626c626c7a6462647864604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0de4eaed8dac8be7c507000000000000000000000000c080040000a09119000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c64626c626c7a6462647864604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0de4eaed8dac8be7c5070000000000000000000000000020af151613d218008080000000000000000000000000000080b6a4961b49113d92042bb2add451251800cbfeb412993702000000000000000000000000000000006d492d3792227a240956645ba9a34a300096fd6925326f0400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002ccd9f6789cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e000000a000000020000000405b52cb8da4881e498215d956eaa812cc01", 7109};
/// A recorded bag event of retail: a purchase of 2 Oak Timber.
constexpr Recorded kRetailBuyTimber{"1ec0c133ed36000000000000000000000000000000000000000000c021b1956d13d218000081000000000000000000000000000080959d0ba69e5198b54b5b0ce3c577a506e004875d13993702000000000000000000000000000000002b3b174c3da3306b97b618c68bef4a0dc0090ebb26326f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f000000201a000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c646262647a62607a666464604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0d842aad4dac4cae0b84e7c5074f06000000000000000000000000c080040000a0311a000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a84724062627c646262647a62607a666464604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764eb296c0d842aad4dac4cae0b84e7c5074f060000000000000000000000000020b1956d13d218000081000000000000000000000000000080959d0ba69e5198b54b5b0ce3c577a506e004875d13993702000000000000000000000000000000002b3b174c3da3306b97b618c68bef4a0dc0090ebb26326f04000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000070ef36bf89cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e000000e000000040000000c0cace0553cf28ccdaa52d86f1e2bb52c301", 7189};
/// A recorded bag event of retail: a purchase of a cape with durability 14.
constexpr Recorded kRetailBuyCape{"1ecac1332537000000000000000000000000000000000000000000c0215f006c5612790a95800000000000000000005810000000402cf1ac0065d3bfdb96212de3a791ec2d404eac790a9937020000000000000000000000000000008058e25901caa67fb72d435ac64f23d95b809c58f314326f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f000000a01b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c6462667e6c6262726a766a62627c74604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf4724864604a4ad8c2e6cea6d6cad8c6eae4744c407646b2acc6d8cae6e0e646aad8e6dad4c0e64280caeac8be7c507000000000000000000000000c080040000a0b11b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeaced6be8adadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c6462667e6c6262726a766a62627c74604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47244604a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf4724864604a4ad8c2e6cea6d6cad8c6eae4744c407646b2acc6d8cae6e0e646aad8e6dad4c0e64280caeac8be7c50700000000000000000000000000205f006c5612790a95800000000000000000005810000000402cf1ac0065d3bfdb96212de3a791ec2d404eac790a9937020000000000000000000000000000008058e25901caa67fb72d435ac64f23d95b809c58f314326f04000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000008f8478689cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001d86010000000020040000a0416f082e000000a0010000200000002096785680b2e9df6dcb9096f1d348f6d601", 7381};
/// A recorded bag event of retail: the stack of a removed listing coming back.
constexpr Recorded kRetailRemovalOne{"9ecac1332737000000000000000000000000000000000000000000c02105002ce2b6790a95000400000000000000000000000000c085674023c81224f213c1cc68ae09d115c0cf303939983702000000000000000000000000000000800bcf8046902548e4278299d15c13a22b809f617272306f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f000000c01b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000c080040000a0d11b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000002005002ce2b6790a95000400000000000000000000000000c085674023c81224f213c1cc68ae09d115c0cf303939983702000000000000000000000000000000800bcf8046902548e4278299d15c13a22b809f617272306f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b29670fc88cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e000000a002000020000000e0c233a011640912f989606634d784e8ca01", 7397};
/// A recorded bag event of retail: the same item coming back, the first of two removals in one session.
constexpr Recorded kRetailRemovalFirst{"9ecac1332737000000000000000000000000000000000000000000c02105002ce2b6790a95000400000000000000000000000000803b3de9c1a7918e5fbae010aaba12a83b405885326697370200000000000000000000000000000000777ad2834f231dbf74c121547525507780b00a65cc2e6f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f000000c01b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000c080040000a0d11b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000002005002ce2b6790a95000400000000000000000000000000803b3de9c1a7918e5fbae010aaba12a83b405885326697370200000000000000000000000000000000777ad2834f231dbf74c121547525507780b00a65cc2e6f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b0fc171e89cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e0000000001000020000000c09d9ef4e0d348c72f5d7008555d09d4dd01", 7397};
/// A recorded bag event of retail: the same item coming back, the second of that session: it tops up the stack of the first.
constexpr Recorded kRetailRemovalSecond{"9ecac1332737000000000000000000000000000000000000000000c02105002ce2b6790a95000400000000000000000000000000803b3de9c1a7918e5fbae010aaba12a83bc0cf30393998370200000000000000000000000000000000777ad2834f231dbf74c1215475255077809f617272306f040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000002000000000000000e01f000000c01b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000c080040000a0d11b000020682cae2e4dae8c0c84270c248dac472489aeac4d04648e2e8fadac472489aeacedabca6decadadedcd4d04a4ad8c2e8c2a0faeac472489aeac8d29cd6d4d04a4ad8c2eeca82a89a847c406c686c66646460666e626c7a686a6a6a6c64604a4ad8c2e2c89aeac2daa2e8c2d8d2eaf47044704a4ad8c2eacc86d0c2dcc8d8ea9ccae8cad47044604a4ad8c2e8caaac0dae4c2ecded0caa4c6eaccc8dae47044604a4ad8c2e8ca84e2e4c2c8d2d8d2eaf47044604a4ad8c2e6cea6d6cad8c6eae4744c40764abc86d0c2dcc8dae8c0c442acdae4c0e44aaceadac8be7c507000000000000000000000000002005002ce2b6790a95000400000000000000000000000000803b3de9c1a7918e5fbae010aaba12a83bc0cf30393998370200000000000000000000000000000000777ad2834f231dbf74c1215475255077809f617272306f0400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000007684a31789cc1b01606f04540a6b093abc801455d6901f500d000000000000000000000020000000001f86010000000020040000a0416f082e0000000001000020000000c09d9ef4e0d348c72f5d7008555d09d4dd01", 7397};

}  // namespace market_material_test

using namespace market_material_test;

// ---- the recorded events and the client's rule ----------------------------------------------------------------------------------------

/// Every call of the PlayerController in a bunch's bits (`server_form` true for the server's).
static std::vector<repl::RpcCall> all_calls_in(const BunchWriter& w, bool server_form) {
    std::vector<repl::RpcCall> out;
    const auto blocks = repl::read_content_blocks(w.data(), w.bit_pos(), 0, server_form);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_pos + b.payload_bits, p);
        if (!calls) continue;
        for (repl::RpcCall& c : *calls) out.push_back(std::move(c));
    }
    return out;
}

/// The one event of a recorded bag answer: it holds exactly one ClientRecieveBatchedStorageEvents call of exactly one event.
static std::optional<repl::ItemStorageEvent> recorded_bag_event(const Recorded& r) {
    const std::vector<repl::RpcCall> calls = all_calls_in(recorded_writer(r), true);
    if (calls.size() != 1) return std::nullopt;
    const std::optional<std::vector<repl::ItemStorageEvent>> events = repl::parse_batched_events(calls[0]);
    if (!events || events->size() != 1) return std::nullopt;
    return events->front();
}

/// The item's name in a pop-up's text: what is in the brackets of the link.
static std::string name_in_toast(const std::string& text) {
    const size_t open = text.find("> [");
    const size_t close = text.find(']', open == std::string::npos ? 0 : open);
    return open == std::string::npos || close == std::string::npos ? std::string() : text.substr(open + 3, close - open - 3);
}

/// Whether `ours` is the recorded event `retail` with only what names this player and this item changed: the item, the character, the storage and the slot, the pop-up's text, its item and
/// the time it was made.
static bool ours_is_the_recorded_event_but_for_names(const repl::ItemStorageEvent& ours, const repl::ItemStorageEvent& retail) {
    if (ours.targets.size() != 1 || retail.targets.size() != 1 || ours.targets[0].slots.size() != 1 || retail.targets[0].slots.size() != 1 || !ours.callout || !retail.callout) return false;
    repl::ItemStorageEvent expected = retail;
    expected.item = ours.item;
    expected.instigator = ours.instigator;
    expected.targets[0].object = ours.targets[0].object;
    expected.targets[0].slots[0] = ours.targets[0].slots[0];
    expected.callout->message = ours.callout->message;
    expected.callout->formatted_on_screen = ours.callout->formatted_on_screen;
    expected.callout->item = ours.callout->item;
    expected.callout->creation_time = ours.callout->creation_time;
    return expected == ours;
}

/// What a client holds of its bags: the stack of each slot of each storage, as the join and the events tell it.
struct ClientHeld {
    uint64_t        record = 0;
    repl::GuidValue guid;
    uint32_t        quantity = 0;
    bool operator==(const ClientHeld&) const = default;
};
using ClientBags = std::map<std::pair<uint64_t, int32_t>, ClientHeld>;

/// The storage object of a container of the session's character: what an event names the storage by.
static uint64_t storage_object_of(const MarketHarness& h, ItemContainer container) {
    const auto& g = h.s->bootstrap().setup().guids;
    switch (container) {
    case ItemContainer::Backpack: return g.backpack.ObjectId;
    case ItemContainer::Resource0: return g.resource_storage[0].ObjectId;
    case ItemContainer::Resource1: return g.resource_storage[1].ObjectId;
    case ItemContainer::Resource2: return g.resource_storage[2].ObjectId;
    default: return 0;
    }
}

/// The bags the server holds, as a client that was sent them at the join holds them.
static ClientBags bags_the_server_holds(const MarketHarness& h) {
    ClientBags out;
    for (const svc::InventoryItem& it : h.s->inventory().items()) {
        const uint64_t object = storage_object_of(h, it.container);
        if (object == 0) continue;
        out[{object, it.slot}] = ClientHeld{static_cast<uint64_t>(it.item_record_id), guid_of_hex(it.instance_guid), static_cast<uint32_t>(it.quantity)};
    }
    return out;
}

/// The client's own rule for an Add (its storage's add: an empty slot takes the item; an occupied slot takes it only when the item stacks with the stack there, named by the guid of the
/// event's slot, and the sum fits the stack's size).  False when it would not apply: the client then asks the server for its storages again, and the player sees the old bags.
static bool client_applies(ClientBags& bags, const std::vector<repl::ItemStorageEvent>& events, uint32_t max_stack) {
    for (const repl::ItemStorageEvent& e : events) {
        if (e.type != static_cast<uint8_t>(repl::StorageEventType::Add) || !e.item.has_item) return false;
        for (const repl::StorageEventContainer& target : e.targets) {
            for (const repl::StorageEventSlot& slot : target.slots) {
                const auto held = bags.find({target.object.ObjectId, slot.slot});
                if (held == bags.end()) {
                    if (slot.count < 1 || static_cast<uint32_t>(slot.count) > max_stack) return false;
                    bags[{target.object.ObjectId, slot.slot}] = ClientHeld{e.item.record_id, slot.item_guid, static_cast<uint32_t>(slot.count)};
                    continue;
                }
                if (held->second.record != e.item.record_id || !(held->second.guid == slot.item_guid) || held->second.quantity + static_cast<uint32_t>(slot.count) > max_stack) return false;
                held->second.quantity += static_cast<uint32_t>(slot.count);
            }
        }
    }
    return true;
}

// ---- the recording ---------------------------------------------------------------------------------------------------------------------

/// The recorded answers: each is one batched call of one Add, and its pop-up is the one the server writes (the text, the item, the type, the size and the chat category, to the bit).
static void test_market_recorded_bag_events_are_one_batched_add_with_a_popup() {
    std::printf("the Marketplace: the recorded bag events are one batched Add with the pop-up\n");
    const Recorded* all[] = {&kRetailBuyTin, &kRetailBuyOakFirst, &kRetailBuyOakSecond, &kRetailBuyTimber, &kRetailBuyCape, &kRetailRemovalOne, &kRetailRemovalFirst, &kRetailRemovalSecond};
    for (const Recorded* r : all) {
        const std::optional<repl::ItemStorageEvent> e = recorded_bag_event(*r);
        CHECK(e.has_value(), "the recorded answer is one ClientRecieveBatchedStorageEvents call of one event");
        if (!e) continue;
        CHECK(e->type == static_cast<uint8_t>(repl::StorageEventType::Add) && e->item.has_item, "it is an Add of an item");
        CHECK(e->instigator.a || e->instigator.b || e->instigator.c || e->instigator.d, "the character is the instigator");
        CHECK(e->move_source.slots.empty() && e->move_source.status == repl::kStorageNoError && !e->extra_guid_a && !e->extra_i64 && !e->extra_guid_b, "it has no source and no extra");
        CHECK(e->targets.size() == 1 && e->targets[0].slots.size() == 1 && e->targets[0].status == repl::kStorageNoError, "it names one storage and one slot");
        if (e->targets.size() != 1 || e->targets[0].slots.size() != 1) continue;
        CHECK(e->targets[0].slots[0].count == static_cast<int32_t>(e->item.quantity) && e->targets[0].slots[0].item_guid == e->item.guid, "the slot's count is the item's units, and the slot names the item's guid");
        CHECK(e->callout.has_value(), "the event carries the pop-up");
        if (!e->callout) continue;
        const std::string name = name_in_toast(e->callout->message);
        const repl::CalloutQueueInfo built = items::acquired_callout(e->item, name, items::default_rarity_facts(), e->callout->creation_time.ticks);
        CHECK(!name.empty() && built == *e->callout, ("the pop-up the server writes for " + name + " is the recorded one, field for field").c_str());
    }
}

/// A stack that joins a held one: the second purchase of a session and the second removal of a session name the slot and the guid of the first one's stack, and carry the units that joined.
static void test_market_recorded_top_up_is_one_add_of_the_units_that_joined() {
    std::printf("the Marketplace: the recorded top-ups are one Add of the units that joined, in the held stack's slot\n");
    const std::optional<repl::ItemStorageEvent> buy1 = recorded_bag_event(kRetailBuyOakFirst), buy2 = recorded_bag_event(kRetailBuyOakSecond);
    const std::optional<repl::ItemStorageEvent> back1 = recorded_bag_event(kRetailRemovalFirst), back2 = recorded_bag_event(kRetailRemovalSecond);
    CHECK(buy1 && buy2 && back1 && back2, "the four events read");
    if (!buy1 || !buy2 || !back1 || !back2) return;
    if (buy1->targets.size() != 1 || buy2->targets.size() != 1 || back1->targets.size() != 1 || back2->targets.size() != 1) return;
    const std::pair<const repl::ItemStorageEvent*, const repl::ItemStorageEvent*> pairs[] = {{&*buy1, &*buy2}, {&*back1, &*back2}};
    for (const auto& pair : pairs) {
        const repl::ItemStorageEvent& first = *pair.first;
        const repl::ItemStorageEvent& second = *pair.second;
        CHECK(first.item.record_id == second.item.record_id && first.targets[0].object == second.targets[0].object, "both are of one item and one storage");
        CHECK(first.targets[0].slots[0].slot == second.targets[0].slots[0].slot && first.targets[0].slots[0].item_guid == second.targets[0].slots[0].item_guid && first.item.guid == second.item.guid,
              "the second Add names the first's slot and guid: it tops that stack up");
        CHECK(second.item.quantity == 1 && second.targets[0].slots[0].count == 1 && second.callout && second.callout->item.quantity == 1, "and carries the one unit that joined, not the stack's new total");
    }
}

// ---- through the session ---------------------------------------------------------------------------------------------------------------

/// One purchase of Coal (1 x 1, stacks of 20) through a session, with everything the tests ask about it.
struct CoalStep {
    std::vector<std::string>            order;   // the PlayerController's calls after the request, by name
    std::vector<repl::ItemStorageEvent> events;  // the events of the batched call
    size_t                              batched = 0, single = 0;
    uint64_t                            response = 99;
    bool                                client_applies_them = false, client_holds_what_server_holds = false;
    int64_t                             coal_before = 0, coal_after = 0, database_coal = 0;
    std::string                         listing, held_guid;
    repl::GuidValue                     listing_guid, item_guid;   // the listing's own guid, and the guid of the stack it holds
};

constexpr int64_t kCoalRecord = cf::resource::kCoal;
constexpr uint32_t kCoalStack = 20;

/// The buyer holds `held` Coal in the slot 7 of the first material bag (none: no Coal), the seller lists `listed` Coal, the buyer buys `bought` of it (the whole stack when the two are equal).
static CoalStep buy_coal(MarketDb& db, int32_t held, int32_t listed, int32_t bought) {
    CoalStep out;
    namespace mk = ::aoc::worldd::market;
    std::vector<svc::InventoryItem> bags = kit();
    if (held > 0) bags.push_back(make_item(ItemContainer::Resource0, 7, kCoalRecord, 700, held));
    const svc::Character buyer = with_start_purse(cyb_record(bags));
    db.store_character(buyer);
    out.listing = db.seed_listing(kSeller, "Brannoc", kNode, kCoalRecord, listed, kUnitPrice);
    out.listing_guid = guid_of_hex(out.listing);
    MarketHarness h(buyer, &db);
    ready_to_trade(h);
    if (const mk::Listing* listing = h.fx.world->market_listings()->find(out.listing_guid)) out.item_guid = guid_of_hex(listing->item.instance_guid);
    out.held_guid = held > 0 ? guid_n(700) : std::string();
    out.coal_before = h.s->inventory().count_of(kCoalRecord);
    ClientBags client = bags_the_server_holds(h);
    const size_t from = h.got.size();
    h.client_request(purchase_request(mk::marketplace_guid(kNode), out.listing_guid, bought), h.pc_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    for (const repl::RpcCall& c : calls) out.order.push_back(c.rpc && c.rpc->name ? c.rpc->name : "?");
    out.batched = count_calls(calls, "ClientRecieveBatchedStorageEvents");
    out.single = count_calls(calls, "ClientReceiveStorageEvent");
    if (const repl::RpcCall* answer = purchase_answer(calls)) out.response = enum_of(*answer, "Response").value_or(99);
    for (const repl::RpcCall& c : calls) {
        if (const auto events = repl::parse_batched_events(c)) out.events = *events;
    }
    out.client_applies_them = client_applies(client, out.events, kCoalStack);
    out.client_holds_what_server_holds = client == bags_the_server_holds(h);
    out.coal_after = h.s->inventory().count_of(kCoalRecord);
    out.database_coal = db.units_of(buyer.id, kCoalRecord);
    return out;
}

static size_t position_of(const std::vector<std::string>& order, const char* name) {
    const auto it = std::find(order.begin(), order.end(), name);
    return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
}

/// What every answer of a done buy or removal is like: one batched call and no single one, retail's order, the popup on every Add, the character as the instigator, the units in the slot's
/// count, and a client that applies the events holds what the server holds.
static void check_the_batched_answer(const CoalStep& s, const char* answer_name, const char* what) {
    const std::string w = what;
    CHECK(s.batched == 1 && s.single == 0, (w + ": the bag event is ONE ClientRecieveBatchedStorageEvents call, not a ClientReceiveStorageEvent").c_str());
    CHECK(position_of(s.order, "ClientRecieveBatchedStorageEvents") < position_of(s.order, answer_name), (w + ": the bag event comes before the answer, as retail's does").c_str());
    CHECK(!s.events.empty(), (w + ": the call holds its events").c_str());
    for (const repl::ItemStorageEvent& e : s.events) {
        CHECK(e.type == static_cast<uint8_t>(repl::StorageEventType::Add) && e.item.has_item && e.targets.size() == 1 && e.targets[0].slots.size() == 1,
              (w + ": each event is an Add of one slot").c_str());
        CHECK(e.instigator == guid_of_hex(kCyb), (w + ": the character is the instigator").c_str());
        CHECK(e.callout.has_value(), (w + ": each Add carries the pop-up").c_str());
        if (!e.callout || e.targets.size() != 1 || e.targets[0].slots.size() != 1) continue;
        CHECK(e.callout->callout_type == items::kAcquiredCalloutType && e.callout->point_size == items::kAcquiredPointSize && e.callout->chat_category == items::kAcquiredChatCategory,
              (w + ": the pop-up is of retail's type, size and chat category").c_str());
        CHECK(e.callout->item == e.item && e.targets[0].slots[0].count == static_cast<int32_t>(e.item.quantity) && e.targets[0].slots[0].item_guid == e.item.guid,
              (w + ": the pop-up names the item of the event, whose units are the slot's count and whose guid is the slot's").c_str());
    }
    CHECK(s.client_applies_them && s.client_holds_what_server_holds, (w + ": a client that applies the events holds exactly what the server holds").c_str());
}

/// The owner's playtest: a material bought onto the stack the buyer holds.  The recorded second buy of a session is the same case.
static void test_market_buy_of_a_material_onto_a_held_stack() {
    std::printf("the Marketplace: a material bought onto a stack the buyer holds\n");
    MarketDb db;
    const CoalStep s = buy_coal(db, 4, 1, 1);
    CHECK(s.response == 0 && s.coal_before == 4 && s.coal_after == 5 && s.database_coal == 5, "the buy is done: the stack of 4 is 5 here and in the database");
    check_the_batched_answer(s, "ClientMarketplaceItemPurchaseResponse", "onto a held stack");
    CHECK(s.order.size() == 3 && s.order[0] == "ClientShowCurrencyCallout", "retail's order: the money popup, the bag event, the answer");
    CHECK(s.events.size() == 1, "one Add for a top-up that fits");
    if (s.events.size() != 1 || s.events[0].targets.size() != 1 || s.events[0].targets[0].slots.size() != 1) return;
    const repl::ItemStorageEvent& e = s.events[0];
    CHECK(e.targets[0].slots[0].slot == 7 && e.targets[0].slots[0].count == 1 && e.targets[0].slots[0].item_guid == guid_of_hex(s.held_guid) && e.item.quantity == 1,
          "the one Add is in the slot of the held stack, under its guid, with the one unit that joined (not the new total)");
    const std::optional<repl::ItemStorageEvent> retail = recorded_bag_event(kRetailBuyOakSecond);
    CHECK(retail && ours_is_the_recorded_event_but_for_names(e, *retail), "and it is the recorded second buy of a session, but for the item, the character, the storage, the slot and the pop-up's text");
}

/// A material bought into an empty slot: the whole stack keeps the listing's guid.
static void test_market_buy_of_a_material_into_an_empty_slot() {
    std::printf("the Marketplace: a material bought with no stack of it held\n");
    MarketDb db;
    const CoalStep s = buy_coal(db, 0, 2, 2);
    CHECK(s.response == 0 && s.coal_before == 0 && s.coal_after == 2 && s.database_coal == 2, "the buy is done: 2 units here and in the database");
    check_the_batched_answer(s, "ClientMarketplaceItemPurchaseResponse", "into an empty slot");
    CHECK(s.events.size() == 1, "one Add for a new stack");
    if (s.events.size() != 1 || s.events[0].targets.size() != 1 || s.events[0].targets[0].slots.size() != 1) return;
    const repl::ItemStorageEvent& e = s.events[0];
    CHECK(e.item.quantity == 2 && e.targets[0].slots[0].count == 2 && e.targets[0].slots[0].item_guid == s.item_guid, "one Add of the 2 units, under the guid of the listed stack: the whole stack keeps it");
    CHECK(e.callout && e.callout->message.find("</>x2") != std::string::npos, "and the pop-up says x2");
    const std::optional<repl::ItemStorageEvent> retail = recorded_bag_event(kRetailBuyTimber);
    CHECK(retail && ours_is_the_recorded_event_but_for_names(e, *retail), "it is the recorded purchase of 2 Oak Timber, but for the item, the character, the storage, the slot and the pop-up's text");
}

/// A part of a stack: the units are a stack of their own, with a new guid.
static void test_market_part_of_a_stack_of_a_material() {
    std::printf("the Marketplace: a part of a stack of a material\n");
    MarketDb db;
    const CoalStep s = buy_coal(db, 0, 5, 2);
    CHECK(s.response == 0 && s.coal_after == 2 && s.database_coal == 2, "the buy is done: 2 units here and in the database");
    check_the_batched_answer(s, "ClientMarketplaceItemPurchaseResponse", "a part of a stack");
    CHECK(s.events.size() == 1, "one Add for the new stack");
    if (s.events.size() != 1 || s.events[0].targets.size() != 1 || s.events[0].targets[0].slots.size() != 1) return;
    const repl::ItemStorageEvent& e = s.events[0];
    CHECK(e.item.quantity == 2 && e.targets[0].slots[0].count == 2 && !(e.targets[0].slots[0].item_guid == s.item_guid), "one Add of the 2 units, under a guid of their own: the listing keeps the stack's guid");
    const std::optional<repl::ItemStorageEvent> retail = recorded_bag_event(kRetailBuyTimber);
    CHECK(retail && ours_is_the_recorded_event_but_for_names(e, *retail), "it is the recorded purchase of 2 Oak Timber, but for the item, the character, the storage, the slot and the pop-up's text");
}

/// A buy that fills the held stack and goes on into a new one: two Adds in the one call, in the order the bags took them.
static void test_market_buy_over_the_end_of_a_stack() {
    std::printf("the Marketplace: a material bought over the end of the stack the buyer holds\n");
    MarketDb db;
    const CoalStep s = buy_coal(db, 19, 3, 3);
    CHECK(s.response == 0 && s.coal_before == 19 && s.coal_after == 22 && s.database_coal == 22, "the buy is done: 19 and 3 are 22 here and in the database");
    check_the_batched_answer(s, "ClientMarketplaceItemPurchaseResponse", "over the end of a stack");
    CHECK(s.events.size() == 2, "two Adds in the one call");
    if (s.events.size() != 2) return;
    if (s.events[0].targets.size() != 1 || s.events[1].targets.size() != 1 || s.events[0].targets[0].slots.size() != 1 || s.events[1].targets[0].slots.size() != 1) return;
    CHECK(s.events[0].targets[0].slots[0].slot == 7 && s.events[0].targets[0].slots[0].count == 1 && s.events[0].targets[0].slots[0].item_guid == guid_of_hex(s.held_guid),
          "the first fills the held stack with the 1 unit that fits");
    CHECK(s.events[1].targets[0].slots[0].slot != 7 && s.events[1].targets[0].slots[0].count == 2 && s.events[1].targets[0].slots[0].item_guid == s.item_guid,
          "the second is a new stack of the other 2 units, under the guid of the listed stack");
    CHECK(s.events[0].callout && s.events[1].callout && s.events[0].callout->message.find("</>x") == std::string::npos && s.events[1].callout->message.find("</>x2") != std::string::npos,
          "and each pop-up names its own units: one, and x2");
}

/// A buy the bags have no room for: no bag event and no money popup, and nothing changes.
static void test_market_a_material_buy_the_bags_refuse_sends_no_bag_event() {
    std::printf("the Marketplace: a material the bags have no room for\n");
    MarketDb db;
    std::vector<svc::InventoryItem> bags = kit();
    bags.push_back(make_item(ItemContainer::Resource0, 0, kCoalRecord, 700, 20));   // a full stack: nothing joins it
    for (int slot = 1; slot < 24; ++slot) bags.push_back(make_item(ItemContainer::Resource0, slot, kCoalRecord, 800 + static_cast<uint32_t>(slot), 20));
    const svc::Character buyer = with_start_purse(cyb_record(bags));
    db.store_character(buyer);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, kCoalRecord, 1, kUnitPrice);
    MarketHarness h(buyer, &db);
    ready_to_trade(h);
    const BuyerState before = state_of(h);
    const size_t from = h.got.size();
    h.client_request(purchase_request(::aoc::worldd::market::marketplace_guid(kNode), guid_of_hex(listing), 1), h.pc_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = purchase_answer(calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 12, "no room: the answer is 12, INVENTORY_FULL");
    CHECK(count_calls(calls, "ClientRecieveBatchedStorageEvents") == 0 && count_calls(calls, "ClientReceiveStorageEvent") == 0 && currency_callout(calls) == nullptr, "no bag event and no money popup");
    CHECK(state_of(h) == before, "the bags and the purse are the same");
}

/// A removal: the stack comes back as one batched Add with the pop-up, before the answer; a stack the seller holds is topped up by it.
static void test_market_removal_of_a_material_listing() {
    std::printf("the Marketplace: a removed listing of a material comes back as the recorded one does\n");
    for (const bool onto_held : {false, true}) {
        MarketDb db;
        std::vector<svc::InventoryItem> bags = kit();
        if (onto_held) bags.push_back(make_item(ItemContainer::Resource0, 7, kCoalRecord, 700, 4));
        const svc::Character record = with_start_purse(cyb_record(bags));
        db.store_character(record);
        const std::string listing = db.seed_listing(record.id, "cyb", kNode, kCoalRecord, onto_held ? 1 : 6, kUnitPrice);
        MarketHarness h(record, &db);
        ready_to_trade(h);
        const int64_t before = h.s->inventory().count_of(kCoalRecord);
        ClientBags client = bags_the_server_holds(h);
        const size_t from = h.got.size();
        h.client_request(removal_request(::aoc::worldd::market::marketplace_guid(kNode), guid_of_hex(listing)), h.pc_ch);
        const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
        CoalStep s;
        for (const repl::RpcCall& c : calls) s.order.push_back(c.rpc && c.rpc->name ? c.rpc->name : "?");
        s.batched = count_calls(calls, "ClientRecieveBatchedStorageEvents");
        s.single = count_calls(calls, "ClientReceiveStorageEvent");
        for (const repl::RpcCall& c : calls) {
            if (const auto events = repl::parse_batched_events(c)) s.events = *events;
        }
        s.client_applies_them = client_applies(client, s.events, kCoalStack);
        s.client_holds_what_server_holds = client == bags_the_server_holds(h);
        const std::string what = onto_held ? "a removal onto a held stack" : "a removal into an empty slot";
        const repl::RpcCall* answer = removal_answer(calls);
        CHECK(answer && enum_of(*answer, "Response").value_or(99) == 0 && h.s->inventory().count_of(kCoalRecord) == before + (onto_held ? 1 : 6) && db.status_of(listing) == market_db::kRemoved,
              (what + ": the stack is back here and the listing is REMOVED in the database").c_str());
        check_the_batched_answer(s, "ClientMarketplaceItemRemovalResponse", what.c_str());
        CHECK(s.order.size() == 2 && s.order[1] == "ClientMarketplaceItemRemovalResponse" && currency_callout(calls) == nullptr, (what + ": the bag event and then the answer, and no money popup: retail's order").c_str());
        if (s.events.size() != 1 || s.events[0].targets.size() != 1 || s.events[0].targets[0].slots.size() != 1) continue;
        const std::optional<repl::ItemStorageEvent> retail = recorded_bag_event(onto_held ? kRetailRemovalSecond : kRetailRemovalOne);
        CHECK(retail && ours_is_the_recorded_event_but_for_names(s.events[0], *retail), (what + ": it is the recorded event, but for the names").c_str());
        CHECK(!onto_held || s.events[0].targets[0].slots[0].slot == 7, "a stack the seller holds is topped up in its own slot");
    }
}

AOC_REGISTER_TEST(test_market_recorded_bag_events_are_one_batched_add_with_a_popup);
AOC_REGISTER_TEST(test_market_recorded_top_up_is_one_add_of_the_units_that_joined);
AOC_REGISTER_TEST(test_market_buy_of_a_material_onto_a_held_stack);
AOC_REGISTER_TEST(test_market_buy_of_a_material_into_an_empty_slot);
AOC_REGISTER_TEST(test_market_part_of_a_stack_of_a_material);
AOC_REGISTER_TEST(test_market_buy_over_the_end_of_a_stack);
AOC_REGISTER_TEST(test_market_a_material_buy_the_bags_refuse_sends_no_bag_event);
AOC_REGISTER_TEST(test_market_removal_of_a_material_listing);
