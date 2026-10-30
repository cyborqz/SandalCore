// ============================================================================
//  quest/bootstrap_objects.inc
//
//  Quests part 3, slice 1 (quest objects): the pawn's answer when the use of a quest object ends.  Included at the end of bootstrap/bootstrap.cpp.
// ============================================================================

std::optional<Send> PlayerBootstrap::interact_done(const emit::FIntrepidNetworkGUID& component, const emit::FIntrepidNetworkGUID& actor) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pawn_.obj.schema->rpc("ClientOnDoInteract");
    if (!rpc) return std::nullopt;
    repl::RpcParams params;
    if (!repl::set_rpc_param(*rpc, params, "Source", object(component)) || !repl::set_rpc_param(*rpc, params, "InInteractable", object(actor))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOnDoInteract";
    call.params = std::move(params);
    return rpcs(Role::Pawn, "ClientOnDoInteract", std::span<const repl::RpcRequest>(&call, 1));
}
