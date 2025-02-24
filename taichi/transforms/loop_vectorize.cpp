// The loop vectorizer

#include "taichi/program/program.h"
#include "taichi/ir/ir.h"
#include "taichi/ir/type_factory.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/visitors.h"

TLANG_NAMESPACE_BEGIN

// Lower Expr tree to a bunch of binary/unary(binary/unary) statements
// Goal: eliminate Expression, and mutable local variables. Make AST SSA.
class LoopVectorize : public IRVisitor {
 public:
  int vectorize;
  Stmt *loop_var;  // an alloca...
  const CompileConfig &config;

  explicit LoopVectorize(const CompileConfig &config) : config(config) {
    allow_undefined_visitor = true;
    invoke_default_visitor = true;
    loop_var = nullptr;
    vectorize = 1;
  }

  static void widen_type(DataType &type, int width) {
    if (width != 1) {
      type = Program::get_type_factory().get_vector_type(width, type);
    }
  }

  void visit(Stmt *stmt) override {
    widen_type(stmt->ret_type, vectorize);
  }

  void visit(ConstStmt *stmt) override {
    stmt->val.repeat(vectorize);
    widen_type(stmt->ret_type, vectorize);
  }

  void visit(Block *stmt_list) override {
    std::vector<Stmt *> statements;
    for (auto &stmt : stmt_list->statements) {
      statements.push_back(stmt.get());
    }
    for (auto stmt : statements) {
      stmt->accept(this);
    }
  }

  void visit(GlobalPtrStmt *ptr) override {
    ptr->snodes.repeat(vectorize);
    widen_type(ptr->ret_type, vectorize);
  }

  void visit(AllocaStmt *alloca) override {
    widen_type(alloca->ret_type, vectorize);
  }

  void visit(SNodeOpStmt *stmt) override {
    if (vectorize == 1)
      return;
    // TI_NOT_IMPLEMENTED;
    /*
    stmt->snodes.repeat(vectorize);
    stmt->ret_type.width *= vectorize;
    */
  }

  void visit(ElementShuffleStmt *stmt) override {
    if (vectorize == 1)
      return;
    int original_width = stmt->width();
    widen_type(stmt->ret_type, vectorize);
    stmt->elements.repeat(vectorize);
    // TODO: this can be buggy
    int stride = stmt->elements[original_width - 1].index + 1;
    if (stmt->elements[0].stmt->width() != 1) {
      for (int i = 0; i < vectorize; i++) {
        for (int j = 0; j < original_width; j++) {
          stmt->elements[i * original_width + j].index += i * stride;
        }
      }
    }
  }

  void visit(LocalLoadStmt *stmt) override {
    if (vectorize == 1)
      return;
    int original_width = stmt->width();
    widen_type(stmt->ret_type, vectorize);
    stmt->src.repeat(vectorize);
    // TODO: this can be buggy
    int stride = stmt->src[original_width - 1].offset + 1;
    if (stmt->src[0].var->width() != 1) {
      for (int i = 0; i < vectorize; i++) {
        for (int j = 0; j < original_width; j++) {
          stmt->src[i * original_width + j].offset += i * stride;
        }
      }
    }
    if (loop_var && stmt->same_source() && stmt->src[0].var == loop_var) {
      // insert_before_me
      LaneAttribute<TypedConstant> const_offsets;
      const_offsets.resize(vectorize * original_width);
      for (int i = 0; i < vectorize * original_width; i++) {
        const_offsets[i] = TypedConstant(i / original_width);
      }
      auto offsets = std::make_unique<ConstStmt>(const_offsets);
      auto add_op = std::make_unique<BinaryOpStmt>(BinaryOpType::add, stmt,
                                                   offsets.get());
      irpass::type_check(add_op.get(), config);
      auto offsets_p = offsets.get();
      stmt->replace_with(add_op.get());
      stmt->insert_after_me(std::move(offsets));
      offsets_p->insert_after_me(std::move(add_op));
    }
  }

  void visit(IfStmt *if_stmt) override {
    if (if_stmt->true_statements)
      if_stmt->true_statements->accept(this);
    if (if_stmt->false_statements) {
      if_stmt->false_statements->accept(this);
    }
  }

  void visit(RangeForStmt *for_stmt) override {
    auto old_vectorize = for_stmt->vectorize;
    if (for_stmt->vectorize != 1)
      vectorize = for_stmt->vectorize;
    // TODO: RangeForStmt::loop_var is deprecated
    // loop_var = for_stmt->loop_var;
    for_stmt->body->accept(this);
    // loop_var = nullptr;
    vectorize = old_vectorize;
  }

  void visit(StructForStmt *for_stmt) override {
    // TODO: StructForStmt::loop_var is deprecated
    return;
    /*if (for_stmt->loop_vars.empty())
      return;
    auto old_vectorize = for_stmt->vectorize;
    if (for_stmt->vectorize != 1)
      vectorize = for_stmt->vectorize;
    loop_var = for_stmt->loop_vars.back();
    for_stmt->body->accept(this);
    loop_var = nullptr;
    vectorize = old_vectorize;*/
  }

  void visit(MeshForStmt *for_stmt) override {
    return;
  }

  void visit(WhileStmt *stmt) override {
    stmt->body->accept(this);
  }

  static void run(IRNode *node, const CompileConfig &config) {
    LoopVectorize inst(config);
    node->accept(&inst);
  }
};

namespace irpass {

void loop_vectorize(IRNode *root, const CompileConfig &config) {
  TI_AUTO_PROF;
  return LoopVectorize::run(root, config);
}

}  // namespace irpass

TLANG_NAMESPACE_END
