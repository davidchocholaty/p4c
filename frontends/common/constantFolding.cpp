/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "lib/gmputil.h"
#include "constantFolding.h"
#include "ir/configuration.h"
#include "frontends/p4/enumInstance.h"

namespace P4 {

const IR::Expression* DoConstantFolding::getConstant(const IR::Expression* expr) const {
    CHECK_NULL(expr);
    auto cst = get(constants, expr);
    if (cst != nullptr)
        return cst;
    if (expr->is<IR::Constant>())
        return expr;
    if (expr->is<IR::BoolLiteral>())
        return expr;
    if (expr->is<IR::ListExpression>()) {
        auto list = expr->to<IR::ListExpression>();
        for (auto e : *list->components)
            if (getConstant(e) == nullptr)
                return nullptr;
        return list;
    }
    if (typesKnown) {
        auto ei = EnumInstance::resolve(expr, typeMap);
        if (ei != nullptr)
            return expr;
    }

    return nullptr;
}

// This has to be called from a visitor method - it calls getOriginal()
void DoConstantFolding::setConstant(const IR::Node* node, const IR::Expression* result) {
    LOG1("Folding " << node << " to " << result << " (" << result->id << ")");
    constants.emplace(node, result);
    constants.emplace(getOriginal(), result);
}

const IR::Node* DoConstantFolding::postorder(IR::PathExpression* e) {
    if (refMap == nullptr)
        return e;
    auto decl = refMap->getDeclaration(e->path);
    if (decl == nullptr)
        return e;
    auto v = get(constants, decl->getNode());
    if (v == nullptr)
        return e;
    setConstant(e, v);
    if (v->is<IR::ListExpression>())
        return e;
    return v;
}

const IR::Node* DoConstantFolding::postorder(IR::Declaration_Constant* d) {
    auto init = getConstant(d->initializer);
    if (init == nullptr) {
        if (typesKnown)
            ::error("%1%: Cannot evaluate initializer for constant", d->initializer);
        return d;
    }

    if (typesKnown) {
        // If we typechecked we're safe
        setConstant(d, init);
    } else {
        // In fact, this declaration may imply a cast, so the actual value of
        // d is not init, but (d->type)init.  The typechecker inserts casts,
        // but if we run this before typechecking we have to be more conservative.
        if (init->is<IR::Constant>()) {
            auto cst = init->to<IR::Constant>();
            if (d->type->is<IR::Type_Bits>()) {
                if (cst->type->is<IR::Type_InfInt>() ||
                    (cst->type->is<IR::Type_Bits>() &&
                     !(*d->type->to<IR::Type_Bits>() == *cst->type->to<IR::Type_Bits>())))
                    init = new IR::Constant(init->srcInfo, d->type, cst->value, cst->base);
                setConstant(d, init);
            }
        }
    }
    if (init != d->initializer)
        d = new IR::Declaration_Constant(d->srcInfo, d->name, d->annotations, d->type, init);
    return d;
}

const IR::Node* DoConstantFolding::postorder(IR::Cmpl* e) {
    auto op = getConstant(e->expr);
    if (op == nullptr)
        return e;

    auto cst = op->to<IR::Constant>();
    if (cst == nullptr) {
        ::error("%1%: Expected an integer value", op);
        return e;
    }
    const IR::Type* t = op->type;
    if (t->is<IR::Type_InfInt>()) {
        ::error("%1%: Operation cannot be applied to values with unknown width;\n"
                "please specify width explicitly", e);
        return e;
    }
    auto tb = t->to<IR::Type_Bits>();
    if (tb == nullptr) {
        if (typesKnown)
            ::error("%1%: Operation can only be applied to int<> or bit<> types", e);
        return e;
    }

    mpz_class value = ~cst->value;
    auto result = new IR::Constant(cst->srcInfo, t, value, cst->base, true);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::Neg* e) {
    auto op = getConstant(e->expr);
    if (op == nullptr)
        return e;

    auto cst = op->to<IR::Constant>();
    if (cst == nullptr) {
        ::error("%1%: Expected an integer value", op);
        return e;
    }
    const IR::Type* t = op->type;
    if (t->is<IR::Type_InfInt>())
        return new IR::Constant(cst->srcInfo, t, -cst->value, cst->base);

    auto tb = t->to<IR::Type_Bits>();
    if (tb == nullptr) {
        if (typesKnown)
            ::error("%1%: Operation can only be applied to int<> or bit<> types", e);
        return e;
    }

    mpz_class value = -cst->value;
    auto result = new IR::Constant(cst->srcInfo, t, value, cst->base, true);
    setConstant(e, result);
    return result;
}

const IR::Constant*
DoConstantFolding::cast(const IR::Constant* node, unsigned base, const IR::Type_Bits* type) const {
    return new IR::Constant(node->srcInfo, type, node->value, base);
}

const IR::Node* DoConstantFolding::postorder(IR::Add* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a + b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Sub* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a - b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Mul* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a * b; });
}

const IR::Node* DoConstantFolding::postorder(IR::BXor* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a ^ b; });
}

const IR::Node* DoConstantFolding::postorder(IR::BAnd* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a & b; });
}

const IR::Node* DoConstantFolding::postorder(IR::BOr* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a | b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Equ* e) {
    return compare(e);
}

const IR::Node* DoConstantFolding::postorder(IR::Neq* e) {
    return compare(e);
}

const IR::Node* DoConstantFolding::postorder(IR::Lss* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a < b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Grt* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a > b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Leq* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a <= b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Geq* e) {
    return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a >= b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Div* e) {
    return binary(e, [e](mpz_class a, mpz_class b) -> mpz_class {
            if (sgn(a) < 0 || sgn(b) < 0) {
                ::error("%1%: Division is not defined for negative numbers", e);
                return 0;
            }
            if (sgn(b) == 0) {
                ::error("%1%: Division by zero", e);
                return 0;
            }
            return a / b;
        });
}

const IR::Node* DoConstantFolding::postorder(IR::Mod* e) {
    return binary(e, [e](mpz_class a, mpz_class b) -> mpz_class {
            if (sgn(a) < 0 || sgn(b) < 0) {
                ::error("%1%: Modulo is not defined for negative numbers", e);
                return 0;
            }
            if (sgn(b) == 0) {
                ::error("%1%: Modulo by zero", e);
                return 0;
            }
            return a % b; });
}

const IR::Node* DoConstantFolding::postorder(IR::Shr* e) {
    return shift(e);
}

const IR::Node* DoConstantFolding::postorder(IR::Shl* e) {
    return shift(e);
}

const IR::Node*
DoConstantFolding::compare(const IR::Operation_Binary* e) {
    auto eleft = getConstant(e->left);
    auto eright = getConstant(e->right);
    if (eleft == nullptr || eright == nullptr)
        return e;

    bool eqTest = e->is<IR::Equ>();
    if (eleft->is<IR::BoolLiteral>()) {
        auto left = eleft->to<IR::BoolLiteral>();
        auto right = eright->to<IR::BoolLiteral>();
        if (left == nullptr || right == nullptr) {
            ::error("%1%: both operands must be Boolean", e);
            return e;
        }
        bool bresult = (left->value == right->value) == eqTest;
        auto result = new IR::BoolLiteral(e->srcInfo, bresult);
        setConstant(e, result);
        return result;
    }

    if (eqTest)
        return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a == b; });
    else
        return binary(e, [](mpz_class a, mpz_class b) -> mpz_class { return a != b; });
}

const IR::Node*
DoConstantFolding::binary(const IR::Operation_Binary* e,
                        std::function<mpz_class(mpz_class, mpz_class)> func) {
    auto eleft = getConstant(e->left);
    auto eright = getConstant(e->right);
    if (eleft == nullptr || eright == nullptr)
        return e;

    auto left = eleft->to<IR::Constant>();
    if (left == nullptr) {
        ::error("%1%: Expected a bit<> or int<> value", e->left);
        return e;
    }
    auto right = eright->to<IR::Constant>();
    if (right == nullptr) {
        ::error("%1%: Expected an bit<> or int<> value", e->right);
        return e;
    }

    const IR::Type* lt = left->type;
    const IR::Type* rt = right->type;
    bool lunk = lt->is<IR::Type_InfInt>();
    bool runk = rt->is<IR::Type_InfInt>();

    const IR::Type* resultType;
    mpz_class value = func(left->value, right->value);

    const IR::Type_Bits* ltb = nullptr;
    const IR::Type_Bits* rtb = nullptr;
    if (!lunk) {
        ltb = lt->to<IR::Type_Bits>();
        if (ltb == nullptr) {
            if (typesKnown)
                ::error("%1%: Operation can only be applied to int<> or bit<> types", e);
            return e;
        }
    }
    if (!runk) {
        rtb = rt->to<IR::Type_Bits>();
        if (rtb == nullptr) {
            if (typesKnown)
                ::error("%1%: Operation can only be applied to int<> or bit<> types", e);
            return e;
        }
    }

    if (!lunk && !runk) {
        // both typed
        if (!ltb->operator==(*rtb)) {
            ::error("%1%: operands have different types: %2% and %3%",
                    e, ltb->toString(), rtb->toString());
            return e;
        }
        resultType = rtb;
    } else if (lunk && runk) {
        resultType = lt;  // i.e., Type_InfInt
    } else {
        // must cast one to the type of the other
        if (lunk) {
            resultType = rtb;
            left = cast(left, left->base, rtb);
        } else {
            resultType = ltb;
            right = cast(right, left->base, ltb);
        }
    }

    const IR::Expression* result;
    if (e->is<IR::Operation_Relation>())
        result = new IR::BoolLiteral(e->srcInfo, static_cast<bool>(value));
    else
        result = new IR::Constant(e->srcInfo, resultType, value, left->base, true);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::LAnd* e) {
    auto left = getConstant(e->left);
    if (left == nullptr)
        return e;

    auto lcst = left->to<IR::BoolLiteral>();
    if (lcst == nullptr) {
        ::error("%1%: Expected a boolean value", left);
        return e;
    }

    if (lcst->value) {
        setConstant(e, e->right);
        return e->right;
    }

    // Short-circuit folding
    auto result = new IR::BoolLiteral(left->srcInfo, false);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::LOr* e) {
    auto left = getConstant(e->left);
    if (left == nullptr)
        return e;

    auto lcst = left->to<IR::BoolLiteral>();
    if (lcst == nullptr) {
        ::error("%1%: Expected a boolean value", left);
        return e;
    }

    if (!lcst->value) {
        setConstant(e, e->right);
        return e->right;
    }

    // Short-circuit folding
    auto result = new IR::BoolLiteral(left->srcInfo, true);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::Slice* e) {
    const IR::Expression* msb = getConstant(e->e1);
    const IR::Expression* lsb = getConstant(e->e2);
    if (msb == nullptr || lsb == nullptr) {
        ::error("%1%: bit indexes must be compile-time constants", e);
        return e;
    }

    if (!typesKnown)
        return e;
    auto e0 = getConstant(e->e0);
    if (e0 == nullptr)
        return e;

    auto cmsb = msb->to<IR::Constant>();
    if (cmsb == nullptr) {
        ::error("%1%: Expected an integer value", msb);
        return e;
    }
    auto clsb = lsb->to<IR::Constant>();
    if (clsb == nullptr) {
        ::error("%1%: Expected an integer value", lsb);
        return e;
    }
    auto cbase = e0->to<IR::Constant>();
    if (cbase == nullptr) {
        ::error("%1%: Expected an integer value", e->e0);
        return e;
    }

    int m = cmsb->asInt();
    int l = clsb->asInt();
    if (m < l) {
        ::error("%1%: bit slicing should be specified as [msb:lsb]", e);
        return e;
    }
    if (m > P4CConfiguration::MaximumWidthSupported ||
        l > P4CConfiguration::MaximumWidthSupported) {
        ::error("%1%: Compiler only supports widths up to %2%",
                e, P4CConfiguration::MaximumWidthSupported);
        return e;
    }
    mpz_class value = cbase->value >> l;
    mpz_class mask = 1;
    mask = mask << (m - l + 1) - 1;
    value = value & mask;
    auto resultType = typeMap->getType(getOriginal(), true);
    if (!resultType->is<IR::Type_Bits>())
        BUG("Type of slice is not Type_Bits, but %1%", resultType);
    auto result = new IR::Constant(e->srcInfo, resultType, value, cbase->base, true);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::Member* e) {
    if (!typesKnown)
        return e;
    auto type = typeMap->getType(e->expr, true);
    auto origtype = typeMap->getType(getOriginal());

    const IR::Expression* result;
    if (type->is<IR::Type_Stack>() && e->member == IR::Type_Stack::arraySize) {
        auto st = type->to<IR::Type_Stack>();
        auto size = st->getSize();
        result = new IR::Constant(st->size->srcInfo, size);
    } else {
        auto expr = getConstant(e->expr);
        if (expr == nullptr)
            return e;
        if (!type->is<IR::Type_StructLike>())
            BUG("Expected a struct type, got %1%", type);
        if (!expr->is<IR::ListExpression>())
            BUG("Expected a list of constants, got %1%", expr);

        auto list = expr->to<IR::ListExpression>();
        auto structType = type->to<IR::Type_StructLike>();

        bool found = false;
        int index = 0;
        for (auto f : *structType->fields) {
            if (f->name.name == e->member.name) {
                found = true;
                break;
            }
            index++;
        }

        if (!found)
            BUG("Could not find field %1% in type %2%", e->member, type);
        result = list->components->at(index)->clone();
    }
    typeMap->setType(result, origtype);
    typeMap->setCompileTimeConstant(result);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::Concat* e) {
    auto eleft = getConstant(e->left);
    auto eright = getConstant(e->right);
    if (eleft == nullptr || eright == nullptr)
        return e;

    auto left = eleft->to<IR::Constant>();
    if (left == nullptr) {
        ::error("%1%: Expected a bit<> or int<> value", e->left);
        return e;
    }
    auto right = eright->to<IR::Constant>();
    if (right == nullptr) {
        ::error("%1%: Expected an bit<> or int<> value", e->right);
        return e;
    }

    auto lt = left->type->to<IR::Type_Bits>();
    auto rt = right->type->to<IR::Type_Bits>();
    if (lt == nullptr || rt == nullptr) {
        ::error("%1%: both operand widths must be known", e);
        return e;
    }
    if (!lt->operator==(*rt)) {
        ::error("%1%: operands have different types: %2% and %3%",
                e, lt->toString(), rt->toString());
        return e;
    }

    auto resultType = IR::Type_Bits::get(Util::SourceInfo(), lt->size + rt->size, lt->isSigned);
    mpz_class value = Util::shift_left(left->value, static_cast<unsigned>(lt->size)) + right->value;
    auto result = new IR::Constant(e->srcInfo, resultType, value, left->base);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::postorder(IR::LNot* e) {
    auto op = getConstant(e->expr);
    if (op == nullptr)
        return e;

    auto cst = op->to<IR::BoolLiteral>();
    if (cst == nullptr) {
        ::error("%1%: Expected a boolean value", op);
        return e;
    }

    auto result = new IR::BoolLiteral(cst->srcInfo, !cst->value);
    setConstant(e, result);
    return result;
}

const IR::Node* DoConstantFolding::shift(const IR::Operation_Binary* e) {
    auto right = getConstant(e->right);
    if (right == nullptr)
        return e;

    auto cr = right->to<IR::Constant>();
    if (cr == nullptr) {
        ::error("%1%: Expected an integer value", right);
        return e;
    }
    if (sgn(cr->value) < 0) {
        ::error("%1%: Shifts with negative amounts are not permitted", e);
        return e;
    }

    if (sgn(cr->value) == 0) {
        // ::warning("%1% with zero", e);
        setConstant(e, e->left);
        return e->left;
    }

    auto left = getConstant(e->left);
    if (left == nullptr)
        return e;

    auto cl = left->to<IR::Constant>();
    if (cl == nullptr) {
        ::error("%1%: Expected an integer value", left);
        return e;
    }

    mpz_class value = cl->value;
    unsigned shift = static_cast<unsigned>(cr->asInt());

    auto tb = left->type->to<IR::Type_Bits>();
    if (tb != nullptr) {
        if (((unsigned)tb->size < shift) && warnings)
            ::warning("%1%: Shifting %2%-bit value with %3%", e, tb->size, shift);
    }

    if (e->is<IR::Shl>())
        value = Util::shift_left(value, shift);
    else
        value = Util::shift_right(value, shift);
    auto result = new IR::Constant(e->srcInfo, left->type, value, cl->base);
    setConstant(e, result);
    return result;
}

const IR::Node *DoConstantFolding::postorder(IR::Cast *e) {
    auto expr = getConstant(e->expr);
    if (expr == nullptr)
        return e;

    const IR::Type* etype;
    if (typesKnown)
        etype = typeMap->getType(getOriginal(), true);
    else
        etype = e->type;

    if (etype->is<IR::Type_Bits>()) {
        auto type = etype->to<IR::Type_Bits>();
        if (expr->is<IR::Constant>()) {
            auto arg = expr->to<IR::Constant>();
            auto result = cast(arg, arg->base, type);
            setConstant(e, result);
            return result;
        } else {
            BUG_CHECK(expr->is<IR::BoolLiteral>(), "%1%: expected a boolean literal", expr);
            auto arg = expr->to<IR::BoolLiteral>();
            int v = arg->value ? 1 : 0;
            auto result = new IR::Constant(e->srcInfo, type, v, 10);
            setConstant(e, result);
            return result;
        }
    } else if (etype->is<IR::Type_StructLike>()) {
        auto result = expr->clone();
        auto origtype = typeMap->getType(getOriginal());
        typeMap->setType(result, origtype);
        typeMap->setCompileTimeConstant(result);
        setConstant(e, result);
        return result;
    }

    return e;
}

DoConstantFolding::Result
DoConstantFolding::setContains(const IR::Expression* keySet, const IR::Expression* select) const {
    if (keySet->is<IR::DefaultExpression>())
        return Result::Yes;
    if (select->is<IR::ListExpression>()) {
        auto list = select->to<IR::ListExpression>();
        if (keySet->is<IR::ListExpression>()) {
            auto klist = keySet->to<IR::ListExpression>();
            BUG_CHECK(list->components->size() == klist->components->size(),
                      "%1% and %2% size mismatch", list, klist);
            for (unsigned i=0; i < list->components->size(); i++) {
                auto r = setContains(klist->components->at(i), list->components->at(i));
                if (r == Result::DontKnow || r == Result::No)
                    return r;
            }
            return Result::Yes;
        } else {
            BUG_CHECK(list->components->size() == 1, "%1%: mismatch in list size", list);
            return setContains(keySet, list->components->at(0));
        }
    }

    if (select->is<IR::BoolLiteral>()) {
        auto key = getConstant(keySet);
        if (key == nullptr)
            ::error("%1%: expression must evaluate to a constant", key);
        BUG_CHECK(key->is<IR::BoolLiteral>(), "%1%: expected a boolean", key);
        if (select->to<IR::BoolLiteral>()->value == key->to<IR::BoolLiteral>()->value)
            return Result::Yes;
        return Result::No;
    }

    BUG_CHECK(select->is<IR::Constant>(), "%1%: expected a constant", select);
    auto cst = select->to<IR::Constant>();
    if (keySet->is<IR::Constant>()) {
        if (keySet->to<IR::Constant>()->value == cst->value)
            return Result::Yes;
        return Result::No;
    } else if (keySet->is<IR::Range>()) {
        auto range = keySet->to<IR::Range>();
        auto left = getConstant(range->left);
        if (left == nullptr) {
            ::error("%1%: expression must evaluate to a constant", left);
            return Result::DontKnow;
        }
        auto right = getConstant(range->right);
        if (right == nullptr) {
            ::error("%1%: expression must evaluate to a constant", right);
            return Result::DontKnow;
        }
        if (left->to<IR::Constant>()->value <= cst->value &&
            right->to<IR::Constant>()->value >= cst->value)
            return Result::Yes;
        return Result::No;
    } else if (keySet->is<IR::Mask>()) {
        // check if left & right == cst & right
        auto range = keySet->to<IR::Mask>();
        auto left = getConstant(range->left);
        if (left == nullptr) {
            ::error("%1%: expression must evaluate to a constant", left);
            return Result::DontKnow;
        }
        auto right = getConstant(range->right);
        if (right == nullptr) {
            ::error("%1%: expression must evaluate to a constant", right);
            return Result::DontKnow;
        }
        if ((left->to<IR::Constant>()->value & right->to<IR::Constant>()->value) ==
            (right->to<IR::Constant>()->value & cst->value))
            return Result::Yes;
        return Result::No;
    }
    ::error("%1%: unexpected expression", keySet);
    return Result::DontKnow;
}

const IR::Node* DoConstantFolding::postorder(IR::SelectExpression* expression) {
    if (!typesKnown) return expression;
    auto sel = getConstant(expression->select);
    if (sel == nullptr)
        return expression;

    IR::Vector<IR::SelectCase> cases;
    bool someUnknown = false;
    bool changes = false;
    bool finished = false;

    const IR::Expression* result = expression;
    /* FIXME -- should erase/replace each element as needed, rather than creating a new Vector.
     * Should really implement this in SelectCase pre/postorder and this postorder goes away */
    for (auto c : expression->selectCases) {
        if (finished) {
            if (warnings)
                ::warning("%1%: unreachable case", c);
            continue;
        }
        auto inside = setContains(c->keyset, sel);
        if (inside == Result::No) {
            changes = true;
            continue;
        } else if (inside == Result::DontKnow) {
            someUnknown = true;
            cases.push_back(c);
        } else {
            changes = true;
            finished = true;
            if (someUnknown) {
                auto newc = new IR::SelectCase(
                    c->srcInfo, new IR::DefaultExpression(Util::SourceInfo()), c->state);
                cases.push_back(newc);
            } else {
                // This is the result.
                result = c->state;
            }
        }
    }

    if (changes) {
        if (cases.size() == 0 && result == expression && warnings)
            // TODO: this is the same as verify(false, error.NoMatch),
            // but we cannot replace the selectExpression with a method call.
            ::warning("%1%: no case matches", expression);
        expression->selectCases = std::move(cases);
    }
    return result;
}

}  // namespace P4
