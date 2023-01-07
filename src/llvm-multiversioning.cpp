// This file is a part of Julia. License is MIT: https://julialang.org/license

// Function multi-versioning
// LLVM pass to clone function for different archs

#include "llvm-version.h"
#include "passes.h"

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

#include <llvm/Pass.h>
#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "julia.h"
#include "julia_internal.h"
#include "processor.h"
#include "support/dtypes.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "llvm-codegen-shared.h"
#include "julia_assert.h"

#define DEBUG_TYPE "julia_multiversioning"
#undef DEBUG

using namespace llvm;

extern Optional<bool> always_have_fma(Function&);

void replaceUsesWithLoad(Function &F, function_ref<GlobalVariable *(Instruction &I)> should_replace, MDNode *tbaa_const);

namespace {
constexpr uint32_t clone_mask =
    JL_TARGET_CLONE_LOOP | JL_TARGET_CLONE_SIMD | JL_TARGET_CLONE_MATH | JL_TARGET_CLONE_CPU | JL_TARGET_CLONE_FLOAT16;

// Treat identical mapping as missing and return `def` in that case.
// We mainly need this to identify cloned function using value map after LLVM cloning
// functions fills the map with identity entries.
template<typename T>
Value *map_get(T &&vmap, Value *key, Value *def=nullptr)
{
    auto val = vmap.lookup(key);
    if (!val || key == val)
        return def;
    return val;
}

static bool is_vector(FunctionType *ty)
{
    if (ty->getReturnType()->isVectorTy())
        return true;
    for (auto arg: ty->params()) {
        if (arg->isVectorTy()) {
            return true;
        }
    }
    return false;
}

static uint32_t collect_func_info(Function &F, bool &has_veccall)
{
    DominatorTree DT(F);
    LoopInfo LI(DT);
    uint32_t flag = 0;
    if (!LI.empty())
        flag |= JL_TARGET_CLONE_LOOP;
    if (is_vector(F.getFunctionType())) {
        flag |= JL_TARGET_CLONE_SIMD;
        has_veccall = true;
    }
    for (auto &bb: F) {
        for (auto &I: bb) {
            if (auto call = dyn_cast<CallInst>(&I)) {
                if (is_vector(call->getFunctionType())) {
                    has_veccall = true;
                    flag |= JL_TARGET_CLONE_SIMD;
                }
                if (auto callee = call->getCalledFunction()) {
                    auto name = callee->getName();
                    if (name.startswith("llvm.muladd.") || name.startswith("llvm.fma.")) {
                        flag |= JL_TARGET_CLONE_MATH;
                    }
                    else if (name.startswith("julia.cpu.")) {
                        if (name.startswith("julia.cpu.have_fma.")) {
                            // for some platforms we know they always do (or don't) support
                            // FMA. in those cases we don't need to clone the function.
                            if (!always_have_fma(*callee).hasValue())
                                flag |= JL_TARGET_CLONE_CPU;
                        } else {
                            flag |= JL_TARGET_CLONE_CPU;
                        }
                    }
                }
            }
            else if (auto store = dyn_cast<StoreInst>(&I)) {
                if (store->getValueOperand()->getType()->isVectorTy()) {
                    flag |= JL_TARGET_CLONE_SIMD;
                }
            }
            else if (I.getType()->isVectorTy()) {
                flag |= JL_TARGET_CLONE_SIMD;
            }
            if (auto mathOp = dyn_cast<FPMathOperator>(&I)) {
                if (mathOp->getFastMathFlags().any()) {
                    flag |= JL_TARGET_CLONE_MATH;
                }
            }

            for (size_t i = 0; i < I.getNumOperands(); i++) {
                if(I.getOperand(i)->getType()->isHalfTy()){
                    flag |= JL_TARGET_CLONE_FLOAT16;
                }
                // Check for BFloat16 when they are added to julia can be done here
            }
            if (has_veccall && (flag & JL_TARGET_CLONE_SIMD) && (flag & JL_TARGET_CLONE_MATH)) {
                return flag;
            }
        }
    }
    return flag;
}

static void annotate_module_clones(Module &M) {
    CallGraph CG(M);
    std::vector<Function *> orig_funcs;
    for (auto &F: M) {
        if (F.isDeclaration())
            continue;
        orig_funcs.push_back(&F);
    }
    bool has_veccall = false;
    auto specs = jl_get_llvm_clone_targets();
    std::vector<APInt> clones(orig_funcs.size(), APInt(specs.size(), 0));
    BitVector subtarget_cloned(orig_funcs.size());
    bool check_relocs = false;

    std::vector<unsigned> func_infos(orig_funcs.size());
    for (unsigned i = 0; i < orig_funcs.size(); i++) {
        func_infos[i] = collect_func_info(*orig_funcs[i], has_veccall);
    }
    for (unsigned i = 1; i < specs.size(); i++) {
        if (specs[i].flags & JL_TARGET_CLONE_ALL) {
            for (unsigned j = 0; j < orig_funcs.size(); j++) {
                clones[j].setBit(i);
            }
            check_relocs = true;
        } else {
            unsigned flag = specs[i].flags & clone_mask;
            std::set<Function*> sets[2];
            for (unsigned j = 0; j < orig_funcs.size(); j++) {
                if (!(func_infos[j] & flag)) {
                    continue;
                }
                sets[0].insert(orig_funcs[j]);
            }
            std::set<Function*> all_origs(sets[0]);
            auto *cur_set = &sets[0];
            auto *next_set = &sets[1];
            // Reduce dispatch by expand the cloning set to functions that are directly called by
            // and calling cloned functions.
            while (!cur_set->empty()) {
                for (auto orig_f: *cur_set) {
                    // Use the uncloned function since it's already in the call graph
                    auto node = CG[orig_f];
                    for (const auto &I: *node) {
                        auto child_node = I.second;
                        auto orig_child_f = child_node->getFunction();
                        if (!orig_child_f)
                            continue;
                        // Already cloned
                        if (all_origs.count(orig_child_f))
                            continue;
                        bool calling_clone = false;
                        for (const auto &I2: *child_node) {
                            auto orig_child_f2 = I2.second->getFunction();
                            if (!orig_child_f2)
                                continue;
                            if (all_origs.count(orig_child_f2)) {
                                calling_clone = true;
                                break;
                            }
                        }
                        if (!calling_clone)
                            continue;
                        next_set->insert(orig_child_f);
                        all_origs.insert(orig_child_f);
                    }
                }
                std::swap(cur_set, next_set);
                next_set->clear();
            }
            for (unsigned j = 0; j < orig_funcs.size(); j++) {
                if (all_origs.count(orig_funcs[j])) {
                    clones[j].setBit(i);
                    subtarget_cloned.set(j);
                }
            }
        }
    }
    if (check_relocs) {
        for (unsigned i = 0; i < orig_funcs.size(); i++) {
            auto &F = *orig_funcs[i];
            if (subtarget_cloned[i] && !ConstantUses<Instruction>(orig_funcs[i], M).done()) {
                F.addFnAttr("julia.mv.reloc", "");
            } else {
                auto uses = ConstantUses<GlobalValue>(orig_funcs[i], M);
                if (!uses.done()) {
                    bool slot = false;
                    for (; !uses.done(); uses.next()) {
                        if (isa<GlobalAlias>(uses.get_info().val)) {
                            slot = true;
                            break;
                        }
                    }
                    if (slot) {
                        F.addFnAttr("julia.mv.reloc", "");
                    } else {
                        F.addFnAttr("julia.mv.fvar", "");
                    }
                }
            }
        }
    }
    SmallString<128> cloneset;
    for (unsigned i = 0; i < orig_funcs.size(); i++) {
        if (!clones[i].isZero()) {
            auto &F = *orig_funcs[i];
            cloneset.clear();
            clones[i].toStringUnsigned(cloneset, 16);
            F.addFnAttr("julia.mv.clones", cloneset);
        }
    }
    if (has_veccall) {
        M.addModuleFlag(Module::Max, "julia.mv.veccall", 1);
    }
}

struct CloneCtx {
    struct Target {
        int idx;
        std::unique_ptr<ValueToValueMapTy> vmap; // ValueToValueMapTy is not movable....
        explicit Target(int idx) :
            idx(idx),
            vmap(new ValueToValueMapTy)
        {
        }
    };
    struct Group : Target {
        std::vector<Target> clones;
        explicit Group(int base) :
            Target(base),
            clones{}
        {}
        Function *base_func(Function *orig_f) const
        {
            if (idx == 0)
                return orig_f;
            return cast<Function>(vmap->lookup(orig_f));
        }

        bool has_subtarget_clone(Function *orig_f) const
        {
            auto base = base_func(orig_f);
            for (auto &clone: clones) {
                if (map_get(*clone.vmap, base))
                    return true;
            }
            return false;
        }
    };
    CloneCtx(Module &M, bool allow_bad_fvars);
    void prepare_slots();
    void clone_decls();
    void clone_bodies();
    void fix_gv_uses();
    void fix_inst_uses();
    void emit_metadata();
private:
    void prepare_vmap(ValueToValueMapTy &vmap);
    void clone_partial(Group &grp, Target &tgt);
    uint32_t get_func_id(Function *F) const;
    std::pair<uint32_t,GlobalVariable*> get_reloc_slot(Function *F) const;
    void rewrite_alias(GlobalAlias *alias, Function* F);

    MDNode *tbaa_const;
    std::vector<jl_target_spec_t> specs;
    std::vector<Group> groups{};
    std::vector<Target *> linearized;
    std::vector<Function*> fvars;
    std::vector<Constant*> gvars;
    Module &M;

    // Map from original function to one based index in `fvars`
    std::map<const Function*,uint32_t> func_ids{};
    std::vector<Function*> orig_funcs{};
    std::vector<uint32_t> func_infos{};
    std::set<Function*> cloned{};
    // GV addresses and their corresponding function id (i.e. 0-based index in `fvars`)
    std::vector<std::pair<Constant*,uint32_t>> gv_relocs{};
    // Mapping from function id (i.e. 0-based index in `fvars`) to GVs to be initialized.
    std::map<uint32_t,GlobalVariable*> const_relocs;
    std::map<Function *, GlobalVariable*> extern_relocs;
    bool allow_bad_fvars{false};
};

template<typename T>
static inline std::vector<T*> consume_gv(Module &M, const char *name, bool allow_bad_fvars)
{
    // Get information about sysimg export functions from the two global variables.
    // Strip them from the Module so that it's easier to handle the uses.
    GlobalVariable *gv = M.getGlobalVariable(name);
    assert(gv && gv->hasInitializer());
    ArrayType *Ty = cast<ArrayType>(gv->getInitializer()->getType());
    unsigned nele = Ty->getArrayNumElements();
    std::vector<T*> res(nele);
    ConstantArray *ary = nullptr;
    if (gv->getInitializer()->isNullValue()) {
        for (unsigned i = 0; i < nele; ++i)
            res[i] = cast<T>(Constant::getNullValue(Ty->getArrayElementType()));
    }
    else {
        ary = cast<ConstantArray>(gv->getInitializer());
        unsigned i = 0;
        while (i < nele) {
            llvm::Value *val = ary->getOperand(i)->stripPointerCasts();
            if (allow_bad_fvars && (!isa<T>(val) || (isa<Function>(val) && cast<Function>(val)->isDeclaration()))) {
                // Shouldn't happen in regular use, but can happen in bugpoint.
                nele--;
                continue;
            }
            res[i++] = cast<T>(val);
        }
        res.resize(nele);
    }
    assert(gv->use_empty());
    gv->eraseFromParent();
    if (ary && ary->use_empty())
        ary->destroyConstant();
    return res;
}

// Collect basic information about targets and functions.
CloneCtx::CloneCtx(Module &M, bool allow_bad_fvars)
    : tbaa_const(tbaa_make_child_with_context(M.getContext(), "jtbaa_const", nullptr, true).first),
      specs(jl_get_llvm_clone_targets()),
      fvars(consume_gv<Function>(M, "jl_fvars", allow_bad_fvars)),
      gvars(consume_gv<Constant>(M, "jl_gvars", false)),
      M(M),
      allow_bad_fvars(allow_bad_fvars)
{
    groups.emplace_back(0);
    linearized.resize(specs.size());
    linearized[0] = &groups[0];
    std::vector<unsigned> group_ids(specs.size(), 0);
    uint32_t ntargets = specs.size();
    for (uint32_t i = 1; i < ntargets; i++) {
        auto &spec = specs[i];
        if (spec.flags & JL_TARGET_CLONE_ALL) {
            group_ids[i] = groups.size();
            groups.emplace_back(i);
        }
        else {
            assert(0 <= spec.base && (unsigned) spec.base < i);
            group_ids[i] = group_ids[spec.base];
            groups[group_ids[i]].clones.emplace_back(i);
        }
    }
    for (auto &grp: groups) {
        for (auto &tgt: grp.clones)
            linearized[tgt.idx] = &tgt;
        linearized[grp.idx] = &grp;
    }
    uint32_t nfvars = fvars.size();
    for (uint32_t i = 0; i < nfvars; i++)
        func_ids[fvars[i]] = i + 1;
    for (auto &F: M) {
        if (F.empty() && !F.hasFnAttribute("julia.mv.clones"))
            continue;
        orig_funcs.push_back(&F);
    }
}

void CloneCtx::prepare_vmap(ValueToValueMapTy &vmap)
{
    // Workaround LLVM `CloneFunctionInfo` bug (?) pre-5.0
    // The `DICompileUnit`s are being cloned but are not added to the `llvm.dbg.cu` metadata
    // which triggers assertions when generating native code/in the verifier.
    // Fix this by forcing an identical mapping for all `DICompileUnit` recorded.
    // The `DISubprogram` cloning on LLVM 5.0 handles this
    // but it doesn't hurt to enforce the identity either.
    auto &MD = vmap.MD();
    for (auto cu: M.debug_compile_units()) {
        MD[cu].reset(cu);
    }
}

void CloneCtx::prepare_slots()
{
    for (auto &F : orig_funcs) {
        if (F->hasFnAttribute("julia.mv.reloc")) {
            assert(F->hasFnAttribute("julia.mv.clones"));
            if (F->isDeclaration()) {
                auto GV = new GlobalVariable(M, F->getType(), false, GlobalValue::ExternalLinkage, nullptr, F->getName() + ".reloc_slot");
                extern_relocs[F] = GV;
            } else {
                auto id = get_func_id(F);
                auto GV = new GlobalVariable(M, F->getType(), false, GlobalValue::InternalLinkage, Constant::getNullValue(F->getType()), F->getName() + ".reloc_slot");
                GV->setVisibility(GlobalValue::HiddenVisibility);
                const_relocs[id] = GV;
            }
        }
    }
}

void CloneCtx::clone_decls()
{
    std::vector<std::string> suffixes(specs.size());
    for (unsigned i = 1; i < specs.size(); i++) {
        suffixes[i] = "." + std::to_string(i);
    }
    for (auto &F : orig_funcs) {
        if (!F->hasFnAttribute("julia.mv.clones"))
            continue;
        APInt clones(specs.size(), F->getFnAttribute("julia.mv.clones").getValueAsString(), 16);
        for (unsigned i = 1; i < specs.size(); i++) {
            if (!clones[i]) {
                continue;
            }
            auto new_F = Function::Create(F->getFunctionType(), F->getLinkage(), F->getName() + suffixes[i], &M);
            new_F->copyAttributesFrom(F);
            new_F->setVisibility(F->getVisibility());
            auto base_func = F;
            if (specs[i].flags & JL_TARGET_CLONE_ALL)
                base_func = static_cast<Group*>(linearized[specs[i].base])->base_func(F);
            (*linearized[i]->vmap)[base_func] = new_F;
        }
    }
}

static void clone_function(Function *F, Function *new_f, ValueToValueMapTy &vmap)
{
    Function::arg_iterator DestI = new_f->arg_begin();
    for (Function::const_arg_iterator J = F->arg_begin(); J != F->arg_end(); ++J) {
        DestI->setName(J->getName());
        vmap[&*J] = &*DestI++;
    }
    SmallVector<ReturnInst*,8> Returns;
#if JL_LLVM_VERSION >= 130000
    // We are cloning into the same module
    CloneFunctionInto(new_f, F, vmap, CloneFunctionChangeType::GlobalChanges, Returns);
#else
    CloneFunctionInto(new_f, F, vmap, true, Returns);
#endif
}

static void add_features(Function *F, StringRef name, StringRef features, uint32_t flags)
{
    auto attr = F->getFnAttribute("target-features");
    if (attr.isStringAttribute()) {
        std::string new_features(attr.getValueAsString());
        new_features += ",";
        new_features += features;
        F->addFnAttr("target-features", new_features);
    }
    else {
        F->addFnAttr("target-features", features);
    }
    F->addFnAttr("target-cpu", name);
    if (!F->hasFnAttribute(Attribute::OptimizeNone)) {
        if (flags & JL_TARGET_OPTSIZE) {
            F->addFnAttr(Attribute::OptimizeForSize);
        }
        else if (flags & JL_TARGET_MINSIZE) {
            F->addFnAttr(Attribute::MinSize);
        }
    }
}

void CloneCtx::clone_bodies()
{
    for (auto F : orig_funcs) {
        for (unsigned i = 0; i < groups.size(); i++) {
            Function *group_F = F;
            if (i != 0) {
                group_F = groups[i].base_func(F);
                if (!F->isDeclaration()) {
                    clone_function(F, group_F, *groups[i].vmap);
                }
            }
            for (auto &target : groups[i].clones) {
                prepare_vmap(*target.vmap);
                auto target_F = cast_or_null<Function>(map_get(*target.vmap, F));
                if (target_F) {
                    if (!F->isDeclaration()) {
                        clone_function(group_F, target_F, *target.vmap);
                    }
                    add_features(target_F, specs[target.idx].cpu_name,
                                specs[target.idx].cpu_features, specs[target.idx].flags);
                    target_F->addFnAttr("julia.mv.clone", std::to_string(i));
                }
            }
            if (i != 0) {
                //TODO should we also do this for target 0?
                add_features(group_F, specs[groups[i].idx].cpu_name,
                            specs[groups[i].idx].cpu_features, specs[groups[i].idx].flags);
            }
            group_F->addFnAttr("julia.mv.clone", std::to_string(i));
        }
    }
}

uint32_t CloneCtx::get_func_id(Function *F) const
{
    auto ref = func_ids.find(F);
    assert(ref != func_ids.end() && "Requesting id of non-fvar!");
    return ref->second - 1;
}

template<typename Stack>
static Constant *rewrite_gv_init(const Stack& stack)
{
    // Null initialize so that LLVM put it in the correct section.
    SmallVector<Constant*, 8> args;
    Constant *res = ConstantPointerNull::get(cast<PointerType>(stack[0].val->getType()));
    uint32_t nlevel = stack.size();
    for (uint32_t i = 1; i < nlevel; i++) {
        auto &frame = stack[i];
        auto val = frame.val;
        Use *use = frame.use;
        unsigned idx = use->getOperandNo();
        unsigned nargs = val->getNumOperands();
        args.resize(nargs);
        for (unsigned j = 0; j < nargs; j++) {
            if (idx == j) {
                args[j] = res;
            }
            else {
                args[j] = cast<Constant>(val->getOperand(j));
            }
        }
        if (auto expr = dyn_cast<ConstantExpr>(val)) {
            res = expr->getWithOperands(args);
        }
        else if (auto ary = dyn_cast<ConstantArray>(val)) {
            res = ConstantArray::get(ary->getType(), args);
        }
        else if (auto strct = dyn_cast<ConstantStruct>(val)) {
            res = ConstantStruct::get(strct->getType(), args);
        }
        else if (isa<ConstantVector>(val)) {
            res = ConstantVector::get(args);
        }
        else {
            jl_safe_printf("Unknown const use.");
            llvm_dump(val);
            abort();
        }
    }
    return res;
}

// replace an alias to a function with a trampoline and (uninitialized) global variable slot
void CloneCtx::rewrite_alias(GlobalAlias *alias, Function *F)
{
    assert(!is_vector(F->getFunctionType()));

    Function *trampoline =
        Function::Create(F->getFunctionType(), alias->getLinkage(), "", &M);
    trampoline->copyAttributesFrom(F);
    trampoline->takeName(alias);
    alias->eraseFromParent();

    uint32_t id;
    GlobalVariable *slot;
    std::tie(id, slot) = get_reloc_slot(F);

    auto BB = BasicBlock::Create(F->getContext(), "top", trampoline);
    IRBuilder<> irbuilder(BB);

    auto ptr = irbuilder.CreateLoad(F->getType(), slot);
    ptr->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_const);
    ptr->setMetadata(llvm::LLVMContext::MD_invariant_load, MDNode::get(F->getContext(), None));

    std::vector<Value *> Args;
    for (auto &arg : trampoline->args())
        Args.push_back(&arg);
    auto call = irbuilder.CreateCall(F->getFunctionType(), ptr, makeArrayRef(Args));
    if (F->isVarArg())
#if (defined(_CPU_ARM_) || defined(_CPU_PPC_) || defined(_CPU_PPC64_))
        abort();    // musttail support is very bad on ARM, PPC, PPC64 (as of LLVM 3.9)
#else
        call->setTailCallKind(CallInst::TCK_MustTail);
#endif
    else
        call->setTailCallKind(CallInst::TCK_Tail);

    if (F->getReturnType() == Type::getVoidTy(F->getContext()))
        irbuilder.CreateRetVoid();
    else
        irbuilder.CreateRet(call);
}

void CloneCtx::fix_gv_uses()
{
    auto single_pass = [&] (Function *orig_f) {
        bool changed = false;
        for (auto uses = ConstantUses<GlobalValue>(orig_f, M); !uses.done(); uses.next()) {
            changed = true;
            auto &stack = uses.get_stack();
            auto info = uses.get_info();
            // We only support absolute pointer relocation.
            assert(info.samebits);
            GlobalVariable *val;
            if (auto alias = dyn_cast<GlobalAlias>(info.val)) {
                rewrite_alias(alias, orig_f);
                continue;
            }
            else {
                val = cast<GlobalVariable>(info.val);
            }
            assert(info.use->getOperandNo() == 0);
            assert(!val->isConstant());
            auto fid = get_func_id(orig_f);
            auto addr = ConstantExpr::getPtrToInt(val, getSizeTy(val->getContext()));
            if (info.offset)
                addr = ConstantExpr::getAdd(addr, ConstantInt::get(getSizeTy(val->getContext()), info.offset));
            gv_relocs.emplace_back(addr, fid);
            val->setInitializer(rewrite_gv_init(stack));
        }
        return changed;
    };
    for (auto orig_f: orig_funcs) {
        if (groups.size() == 1 && !cloned.count(orig_f))
            continue;
        while (single_pass(orig_f)) {
        }
    }
}

std::pair<uint32_t,GlobalVariable*> CloneCtx::get_reloc_slot(Function *F) const
{
    if (F->isDeclaration()) {
        auto extern_decl = extern_relocs.find(F);
        assert(extern_decl != extern_relocs.end() && "Missing extern relocation slot!");
        return {(uint32_t)-1, extern_decl->second};
    } else {
        auto id = get_func_id(F);
        auto slot = const_relocs.find(id);
        assert(slot != const_relocs.end() && "Missing relocation slot!");
        return {id, slot->second};
    }
}

template<typename Stack>
static Value *rewrite_inst_use(const Stack& stack, Value *replace, Instruction *insert_before)
{
    SmallVector<Constant*, 8> args;
    uint32_t nlevel = stack.size();
    for (uint32_t i = 1; i < nlevel; i++) {
        auto &frame = stack[i];
        auto val = frame.val;
        Use *use = frame.use;
        unsigned idx = use->getOperandNo();
        if (auto expr = dyn_cast<ConstantExpr>(val)) {
            auto inst = expr->getAsInstruction();
            inst->replaceUsesOfWith(val->getOperand(idx), replace);
            inst->insertBefore(insert_before);
            replace = inst;
            continue;
        }
        unsigned nargs = val->getNumOperands();
        args.resize(nargs);
        for (unsigned j = 0; j < nargs; j++) {
            auto op = val->getOperand(j);
            if (idx == j) {
                args[j] = UndefValue::get(op->getType());
            }
            else {
                args[j] = cast<Constant>(op);
            }
        }
        if (auto ary = dyn_cast<ConstantArray>(val)) {
            replace = InsertValueInst::Create(ConstantArray::get(ary->getType(), args),
                                              replace, {idx}, "", insert_before);
        }
        else if (auto strct = dyn_cast<ConstantStruct>(val)) {
            replace = InsertValueInst::Create(ConstantStruct::get(strct->getType(), args),
                                              replace, {idx}, "", insert_before);
        }
        else if (isa<ConstantVector>(val)) {
            replace = InsertElementInst::Create(ConstantVector::get(args), replace,
                                                ConstantInt::get(getSizeTy(insert_before->getContext()), idx), "",
                                                insert_before);
        }
        else {
            jl_safe_printf("Unknown const use.");
            llvm_dump(val);
            abort();
        }
    }
    return replace;
}

void CloneCtx::fix_inst_uses()
{
    uint32_t nfuncs = orig_funcs.size();
    for (auto &grp: groups) {
        for (uint32_t i = 0; i < nfuncs; i++) {
            auto orig_f = orig_funcs[i];
            if (!grp.has_subtarget_clone(orig_f))
                continue;
            auto F = grp.base_func(orig_f);
            auto grpidx = std::to_string(grp.idx);
            replaceUsesWithLoad(*F, [&](Instruction &I) -> GlobalVariable * {
                uint32_t id;
                GlobalVariable *slot;
                auto use_f = I.getFunction();
                if (!use_f->hasFnAttribute("julia.mv.clone") || use_f->getFnAttribute("julia.mv.clone").getValueAsString() != grpidx)
                    return nullptr;
                std::tie(id, slot) = get_reloc_slot(orig_f);
                return slot;
            }, tbaa_const);
        }
    }
}

static Constant *get_ptrdiff32(Constant *ptr, Constant *base)
{
    if (ptr->getType()->isPointerTy())
        ptr = ConstantExpr::getPtrToInt(ptr, getSizeTy(ptr->getContext()));
    auto ptrdiff = ConstantExpr::getSub(ptr, base);
    return sizeof(void*) == 8 ? ConstantExpr::getTrunc(ptrdiff, Type::getInt32Ty(ptr->getContext())) : ptrdiff;
}

template<typename T>
static Constant *emit_offset_table(Module &M, const std::vector<T*> &vars, StringRef name, StringRef suffix)
{
    auto T_int32 = Type::getInt32Ty(M.getContext());
    auto T_size = getSizeTy(M.getContext());
    uint32_t nvars = vars.size();
    Constant *base = nullptr;
    if (nvars > 0) {
        base = ConstantExpr::getBitCast(vars[0], T_size->getPointerTo());
        auto ga = GlobalAlias::create(T_size, 0, GlobalVariable::ExternalLinkage,
                                       name + "_base" + suffix,
                                       base, &M);
        ga->setVisibility(GlobalValue::HiddenVisibility);
    } else {
        auto gv = new GlobalVariable(M, T_size, true, GlobalValue::ExternalLinkage, Constant::getNullValue(T_size), name + "_base" + suffix);
        gv->setVisibility(GlobalValue::HiddenVisibility);
        base = gv;
    }
    auto vbase = ConstantExpr::getPtrToInt(base, T_size);
    std::vector<Constant*> offsets(nvars + 1);
    offsets[0] = ConstantInt::get(T_int32, nvars);
    if (nvars > 0) {
        offsets[1] = ConstantInt::get(T_int32, 0);
        for (uint32_t i = 1; i < nvars; i++)
            offsets[i + 1] = get_ptrdiff32(vars[i], vbase);
    }
    ArrayType *vars_type = ArrayType::get(T_int32, nvars + 1);
    auto gv = new GlobalVariable(M, vars_type, true,
                                  GlobalVariable::ExternalLinkage,
                                  ConstantArray::get(vars_type, offsets),
                                  name + "_offsets" + suffix);
    gv->setVisibility(GlobalValue::HiddenVisibility);
    return vbase;
}

void CloneCtx::emit_metadata()
{
    uint32_t nfvars = fvars.size();
    if (allow_bad_fvars && nfvars == 0) {
        // Will result in a non-loadable sysimg, but `allow_bad_fvars` is for bugpoint only
        return;
    }

    StringRef suffix;
    if (auto suffix_md = M.getModuleFlag("julia.mv.suffix")) {
        suffix = cast<MDString>(suffix_md)->getString();
    }

    // Store back the information about exported functions.
    auto fbase = emit_offset_table(M, fvars, "jl_fvar", suffix);
    auto gbase = emit_offset_table(M, gvars, "jl_gvar", suffix);

    M.getGlobalVariable("jl_fvar_idxs")->setName("jl_fvar_idxs" + suffix);
    M.getGlobalVariable("jl_gvar_idxs")->setName("jl_gvar_idxs" + suffix);

    uint32_t ntargets = specs.size();

    // Generate `jl_dispatch_reloc_slots`
    std::set<uint32_t> shared_relocs;
    {
        auto T_int32 = Type::getInt32Ty(M.getContext());
        std::stable_sort(gv_relocs.begin(), gv_relocs.end(),
                         [] (const std::pair<Constant*,uint32_t> &lhs,
                             const std::pair<Constant*,uint32_t> &rhs) {
                             return lhs.second < rhs.second;
                         });
        std::vector<Constant*> values{nullptr};
        uint32_t gv_reloc_idx = 0;
        uint32_t ngv_relocs = gv_relocs.size();
        for (uint32_t id = 0; id < nfvars; id++) {
            // TODO:
            // explicitly set section? so that we are sure the relocation slots
            // are in the same section as `gbase`.
            auto id_v = ConstantInt::get(T_int32, id);
            for (; gv_reloc_idx < ngv_relocs && gv_relocs[gv_reloc_idx].second == id;
                 gv_reloc_idx++) {
                shared_relocs.insert(id);
                values.push_back(id_v);
                values.push_back(get_ptrdiff32(gv_relocs[gv_reloc_idx].first, gbase));
            }
            auto it = const_relocs.find(id);
            if (it != const_relocs.end()) {
                shared_relocs.insert(id);
                values.push_back(id_v);
                values.push_back(get_ptrdiff32(it->second, gbase));
            }
        }
        values[0] = ConstantInt::get(T_int32, values.size() / 2);
        ArrayType *vars_type = ArrayType::get(T_int32, values.size());
        auto gv = new GlobalVariable(M, vars_type, true, GlobalVariable::ExternalLinkage,
                                      ConstantArray::get(vars_type, values),
                                      "jl_clone_slots" + suffix);
        gv->setVisibility(GlobalValue::HiddenVisibility);
    }

    // Generate `jl_dispatch_fvars_idxs` and `jl_dispatch_fvars_offsets`
    {
        std::vector<uint32_t> idxs;
        std::vector<Constant*> offsets;
        for (uint32_t i = 0; i < ntargets; i++) {
            auto tgt = linearized[i];
            auto &spec = specs[i];
            uint32_t len_idx = idxs.size();
            idxs.push_back(0); // We will fill in the real value later.
            uint32_t count = 0;
            if (i == 0 || spec.flags & JL_TARGET_CLONE_ALL) {
                auto grp = static_cast<Group*>(tgt);
                count = jl_sysimg_tag_mask;
                for (uint32_t j = 0; j < nfvars; j++) {
                    if (shared_relocs.count(j)) {
                        count++;
                        idxs.push_back(j);
                    }
                    if (i != 0) {
                        offsets.push_back(get_ptrdiff32(grp->base_func(fvars[j]), fbase));
                    }
                }
            }
            else {
                auto baseidx = spec.base;
                auto grp = static_cast<Group*>(linearized[baseidx]);
                idxs.push_back(baseidx);
                for (uint32_t j = 0; j < nfvars; j++) {
                    auto base_f = grp->base_func(fvars[j]);
                    if (shared_relocs.count(j)) {
                        count++;
                        idxs.push_back(jl_sysimg_tag_mask | j);
                        auto f = map_get(*tgt->vmap, base_f, base_f);
                        offsets.push_back(get_ptrdiff32(cast<Function>(f), fbase));
                    }
                    else if (auto f = map_get(*tgt->vmap, base_f)) {
                        count++;
                        idxs.push_back(j);
                        offsets.push_back(get_ptrdiff32(cast<Function>(f), fbase));
                    }
                }
            }
            idxs[len_idx] = count;
        }
        auto idxval = ConstantDataArray::get(M.getContext(), idxs);
        auto gv1 = new GlobalVariable(M, idxval->getType(), true,
                                      GlobalVariable::ExternalLinkage,
                                      idxval, "jl_clone_idxs" + suffix);
        gv1->setVisibility(GlobalValue::HiddenVisibility);
        ArrayType *offsets_type = ArrayType::get(Type::getInt32Ty(M.getContext()), offsets.size());
        auto gv2 = new GlobalVariable(M, offsets_type, true,
                                      GlobalVariable::ExternalLinkage,
                                      ConstantArray::get(offsets_type, offsets),
                                      "jl_clone_offsets" + suffix);
        gv2->setVisibility(GlobalValue::HiddenVisibility);
    }
}

static bool runMultiVersioning(Module &M, bool allow_bad_fvars)
{
    // Group targets and identify cloning bases.
    // Also initialize function info maps (we'll update these maps as we go)
    // Maps that we need includes,
    //
    //     * Original function -> ID (initialize from `fvars` and allocate ID lazily)
    //     * Cloned function -> Original function (add as we clone functions)
    //     * Original function -> Base function (target specific and updated by LLVM)
    //     * ID -> relocation slots (const).
    if (M.getName() == "sysimage")
        return false;

    GlobalVariable *fvars = M.getGlobalVariable("jl_fvars");
    GlobalVariable *gvars = M.getGlobalVariable("jl_gvars");
    if (allow_bad_fvars && (!fvars || !fvars->hasInitializer() || !isa<ConstantArray>(fvars->getInitializer()) ||
                            !gvars || !gvars->hasInitializer() || !isa<ConstantArray>(gvars->getInitializer())))
        return false;

    CloneCtx clone(M, allow_bad_fvars);

    clone.prepare_slots();

    clone.clone_decls();

    clone.clone_bodies();

    // Scan **ALL** cloned functions (including full cloning for base target)
    // for global variables initialization use.
    // Replace them with `null` slot to be initialized at runtime and record relocation slot.
    // These relocations must be initialized for **ALL** targets.
    clone.fix_gv_uses();

    // For each group, scan all functions cloned by **PARTIALLY** cloned targets for
    // instruction use.
    // A function needs a const relocation slot if it is cloned and is called by a
    // uncloned function for at least one partially cloned target in the group.
    // This is also the condition that a use in an uncloned function needs to be replaced with
    // a slot load (i.e. if both the caller and the callee are always cloned or not cloned
    // on all targets, the caller site does not need a relocation slot).
    // A target needs a slot to be initialized iff at least one caller is not initialized.
    clone.fix_inst_uses();

    // Store back sysimg information with the correct format.
    // At this point, we should have fixed up all the uses of the cloned functions
    // and collected all the shared/target-specific relocations.
    clone.emit_metadata();
#ifdef JL_VERIFY_PASSES
    assert(!verifyModule(M, &errs()));
#endif

    return true;
}

struct MultiVersioningLegacy: public ModulePass {
    static char ID;
    MultiVersioningLegacy(bool allow_bad_fvars=false)
        : ModulePass(ID), allow_bad_fvars(allow_bad_fvars)
    {}

private:
    bool runOnModule(Module &M) override;
    bool allow_bad_fvars;
};

bool MultiVersioningLegacy::runOnModule(Module &M)
{
    return runMultiVersioning(M, allow_bad_fvars);
}


char MultiVersioningLegacy::ID = 0;
static RegisterPass<MultiVersioningLegacy> X("JuliaMultiVersioning", "JuliaMultiVersioning Pass",
                                       false /* Only looks at CFG */,
                                       false /* Analysis Pass */);

} // anonymous namespace

void multiversioning_preannotate(Module &M)
{
    annotate_module_clones(M);
}

void replaceUsesWithLoad(Function &F, function_ref<GlobalVariable *(Instruction &I)> should_replace, MDNode *tbaa_const) {
    bool changed;
    do {
        changed = false;
        for (auto uses = ConstantUses<Instruction>(&F, *F.getParent()); !uses.done(); uses.next()) {
            auto info = uses.get_info();
            auto use_i = info.val;
            GlobalVariable *slot = should_replace(*use_i);
            if (!slot)
                continue;
            Instruction *insert_before = use_i;
            if (auto phi = dyn_cast<PHINode>(use_i))
                insert_before = phi->getIncomingBlock(*info.use)->getTerminator();
            Instruction *ptr = new LoadInst(F.getType(), slot, "", false, insert_before);
            ptr->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_const);
            ptr->setMetadata(llvm::LLVMContext::MD_invariant_load, MDNode::get(ptr->getContext(), None));
            use_i->setOperand(info.use->getOperandNo(),
                                rewrite_inst_use(uses.get_stack(), ptr,
                                                insert_before));
            changed = true;
        }
    } while (changed);
}

PreservedAnalyses MultiVersioning::run(Module &M, ModuleAnalysisManager &AM)
{
    if (runMultiVersioning(M, external_use)) {
        auto preserved = PreservedAnalyses::allInSet<CFGAnalyses>();
        preserved.preserve<LoopAnalysis>();
        return preserved;
    }
    return PreservedAnalyses::all();
}

Pass *createMultiVersioningPass(bool allow_bad_fvars)
{
    return new MultiVersioningLegacy(allow_bad_fvars);
}

extern "C" JL_DLLEXPORT void LLVMExtraAddMultiVersioningPass_impl(LLVMPassManagerRef PM)
{
    unwrap(PM)->add(createMultiVersioningPass(false));
}
