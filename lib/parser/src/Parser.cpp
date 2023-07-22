#include "rlc/parser/Parser.hpp"

#include <system_error>

#include "llvm/Support/Error.h"
#include "rlc/parser/Lexer.hpp"
#include "rlc/utils/Error.hpp"

using namespace llvm;
using namespace rlc;
using namespace std;

[[nodiscard]] mlir::Location Parser::getCurrentSourcePos() const { return pos; }

void Parser::next()
{
	pos = mlir::FileLineColLoc::get(
			ctx,
			fileName,
			lexer.getCurrentLine(),
			std::max<int64_t>(1, lexer.getCurrentColumn() - 1));
	if (current == Token::Identifier)
		lIdent = lexer.lastIndent();
	if (current == Token::Int64)
		lInt64 = lexer.lastInt64();
	if (current == Token::Double)
		lDouble = lexer.lastDouble();
	current = lexer.next();
}

bool Parser::accept(Token t)
{
	if (current != t)
		return false;

	next();
	return true;
}

Expected<Token> Parser::expect(Token t)
{
	if (accept(t))
		return t;

	std::string errorMessage = "\nunexpected token ";
	errorMessage += tokenToString(current);
	errorMessage += " expected ";
	errorMessage += tokenToString(t);
	return make_error<RlcError>(
			std::move(errorMessage),
			RlcErrorCategory::errorCode(RlcErrorCode::unexpectedToken),
			getCurrentSourcePos());
}

#define EXPECT(Token)                                                          \
	if (auto e = expect(Token); !e)                                              \
	return e.takeError()

#define TRY(outVar, expression)                                                \
	auto outVar = expression;                                                    \
	if (!outVar)                                                                 \
	return outVar.takeError()

llvm::Expected<mlir::Value> Parser::builtinFromArray()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordFromArray);
	EXPECT(Token::LAng);
	TRY(type, singleTypeUse());
	EXPECT(Token::RAng);
	EXPECT(Token::LPar);
	TRY(size, expression());
	EXPECT(Token::RPar);

	return builder.create<mlir::rlc::FromByteArrayOp>(location, *type, *size);
}

llvm::Expected<mlir::Value> Parser::builtinToArray()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordToArray);
	EXPECT(Token::LPar);
	TRY(size, expression());
	EXPECT(Token::RPar);

	return builder.create<mlir::rlc::AsByteArrayOp>(
			location, mlir::rlc::UnknownType::get(builder.getContext()), *size);
}

// builtinMalloc : "__builtin_destroy_do_not_use(" expression ")"
Expected<mlir::Operation*> Parser::builtinDestroy()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordDestroy);
	EXPECT(Token::LPar);
	TRY(arg, expression());
	EXPECT(Token::RPar);
	EXPECT(Token::Newline);

	return builder.create<mlir::rlc::DestroyOp>(location, *arg);
}

// builtinMalloc : "__builtin_malloc_do_not_use<" typeUse ">(" expression ")"
Expected<mlir::Value> Parser::builtinMalloc()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordMalloc);
	EXPECT(Token::LAng);
	TRY(type, singleTypeUse());
	EXPECT(Token::RAng);
	EXPECT(Token::LPar);
	TRY(size, expression());
	EXPECT(Token::RPar);

	return builder.create<mlir::rlc::MallocOp>(
			location,
			mlir::rlc::OwningPtrType::get(type->getContext(), *type),
			*size);
}

// builtinFree : "__builtin_free_do_not_use(" expression ")\n"
Expected<mlir::rlc::FreeOp> Parser::builtinFree()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordFree);
	EXPECT(Token::LPar);
	TRY(toDelete, expression());
	EXPECT(Token::RPar);
	EXPECT(Token::Newline);

	return builder.create<mlir::rlc::FreeOp>(location, *toDelete);
}

/**
 * primaryExpression : Ident | Double | int64 | "true" | "false" | "("
 * expression ")"  | builtinMalloc | builtinFromArray | builtinToArray
 */
Expected<mlir::Value> Parser::primaryExpression()
{
	auto location = getCurrentSourcePos();
	if (accept<Token::Identifier>())
		return builder.create<mlir::rlc::UnresolvedReference>(location, lIdent);

	if (accept<Token::Double>())
		return builder.create<mlir::rlc::Constant>(location, lDouble);

	if (accept<Token::Int64>())
		return builder.create<mlir::rlc::Constant>(location, lInt64);

	if (accept<Token::KeywordFalse>())
		return builder.create<mlir::rlc::Constant>(location, false);

	if (accept<Token::KeywordTrue>())
		return builder.create<mlir::rlc::Constant>(location, true);

	if (current == Token::KeywordMalloc)
		return builtinMalloc();

	if (current == Token::KeywordFromArray)
		return builtinFromArray();

	if (current == Token::KeywordToArray)
		return builtinToArray();

	if (accept<Token::LPar>())
	{
		TRY(exp, expression());
		EXPECT(Token::RPar);
		return exp;
	}

	return make_error<RlcError>(
			" empty expression",
			RlcErrorCategory::errorCode(RlcErrorCode::unexpectedToken),
			location);
}

/**
 * argumentExpressionList : (expression",")*expression
 */
Expected<SmallVector<mlir::Value, 3>> Parser::argumentExpressionList()
{
	SmallVector<mlir::Value, 3> exps;
	if (current == Token::RPar)
		return exps;

	do
	{
		TRY(exp, expression());
		exps.emplace_back(std::move(*exp));
	} while (accept<Token::Comma>());

	return exps;
}

mlir::Type Parser::unkType() { return mlir::rlc::UnknownType::get(ctx); }

/**
 * postFixExpression :
 *			((postFixExpression)*
 *					"is" type
 *					|("["expression"]"
 *					|"("argumentExpressionList ")" |
 *					. identifier ["(" argumentExpressionList")"]))
 *			| primaryExpression
 */
Expected<mlir::Value> Parser::postFixExpression()
{
	TRY(maybeExp, primaryExpression());
	auto exp = std::move(*maybeExp);

	while (true)
	{
		auto location = getCurrentSourcePos();
		if (accept<Token::KeywordIs>())
		{
			TRY(type, singleTypeUse());
			exp = builder.create<mlir::rlc::UncheckedIsOp>(location, exp, *type)
								.getResult();
			continue;
		}
		if (accept<Token::LPar>())
		{
			TRY(args, argumentExpressionList());
			EXPECT(Token::RPar);
			exp = builder.create<mlir::rlc::CallOp>(location, unkType(), exp, *args)
								.getResult(0);
			continue;
		}

		if (accept<Token::LSquare>())
		{
			TRY(accExp, expression());
			EXPECT(Token::RSquare);
			exp = builder.create<mlir::rlc::ArrayAccess>(
					location, unkType(), exp, *accExp);
			continue;
		}

		if (accept<Token::Dot>())
		{
			EXPECT(Token::Identifier);
			auto memberName = lIdent;
			if (not accept<Token::LPar>())
			{
				exp = builder.create<mlir::rlc::UnresolvedMemberAccess>(
						location, unkType(), exp, memberName);
				continue;
			}

			TRY(arguments, argumentExpressionList());
			EXPECT(Token::RPar);
			arguments->insert(arguments->begin(), std::move(exp));

			auto ref = builder.create<mlir::rlc::UnresolvedReference>(
					location, unkType(), memberName);

			exp = builder
								.create<mlir::rlc::CallOp>(
										location, unkType(), ref->getResult(0), *arguments)
								.getResult(0);
		}
		break;
	}
	return exp;
}

/**
 * unaryExpression : postFixExpression | unaryOperator unaryExpression
 */
Expected<mlir::Value> Parser::unaryExpression()
{
	auto location = getCurrentSourcePos();
	if (accept<Token::Plus>())
	{
		TRY(exp, unaryExpression());
		return std::move(*exp);
	}

	if (accept<Token::Minus>())
	{
		TRY(exp, unaryExpression());
		return builder.create<mlir::rlc::MinusOp>(location, unkType(), *exp);
	}

	if (accept<Token::ExMark>())
	{
		TRY(exp, unaryExpression());
		return builder.create<mlir::rlc::NotOp>(location, unkType(), *exp);
	}

	return postFixExpression();
}

/**
 * multicativeExpression : unaryExpression | multiplicativeExpresion ('*' | '%'
 * | '/') unaryExpression
 */
Expected<mlir::Value> Parser::multyplicativeExpression()
{
	TRY(exp, unaryExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::Mult>())
	{
		TRY(rhs, multyplicativeExpression());
		return builder.create<mlir::rlc::MultOp>(location, unkType(), *exp, *rhs);
	}
	if (accept<Token::Divide>())
	{
		TRY(rhs, multyplicativeExpression());
		return builder.create<mlir::rlc::DivOp>(location, unkType(), *exp, *rhs);
	}
	if (accept<Token::Module>())
	{
		TRY(rhs, multyplicativeExpression());
		return builder.create<mlir::rlc::ReminderOp>(
				location, unkType(), *exp, *rhs);
	}

	return std::move(*exp);
}

/**
 * additiveExpression : multiplicativeExpression | additiveExpression ('+' |
 * '-')multiplicativeExpression
 */
Expected<mlir::Value> Parser::additiveExpression()
{
	TRY(exp, multyplicativeExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::Plus>())
	{
		TRY(rhs, additiveExpression());
		return builder.create<mlir::rlc::AddOp>(location, unkType(), *exp, *rhs);
	}
	if (accept<Token::Minus>())
	{
		TRY(rhs, additiveExpression());
		return builder.create<mlir::rlc::SubOp>(location, unkType(), *exp, *rhs);
	}
	return std::move(*exp);
}

/**
 * orExpression : additiveExpression (('<' | '>' | '<=' | '>=')
 * additiveExpression)*
 */
Expected<mlir::Value> Parser::relationalExpression()
{
	TRY(exp, additiveExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::LAng>())
	{
		TRY(rhs, relationalExpression());
		return builder.create<mlir::rlc::LessOp>(location, unkType(), *exp, *rhs);
	}
	if (accept<Token::RAng>())
	{
		TRY(rhs, relationalExpression());
		return builder.create<mlir::rlc::GreaterOp>(
				location, unkType(), *exp, *rhs);
	}
	if (accept<Token::GEqual>())
	{
		TRY(rhs, relationalExpression());
		return builder.create<mlir::rlc::GreaterEqualOp>(
				location, unkType(), *exp, *rhs);
	}
	if (accept<Token::LEqual>())
	{
		TRY(rhs, relationalExpression());
		return builder.create<mlir::rlc::LessEqualOp>(
				location, unkType(), *exp, *rhs);
	}
	return std::move(*exp);
}

/**
 * orExpression : relationalExpression (('==' | '!=') relationalExpression)*
 */
Expected<mlir::Value> Parser::equalityExpression()
{
	TRY(exp, relationalExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::EqualEqual>())
	{
		TRY(rhs, equalityExpression());
		return builder.create<mlir::rlc::EqualOp>(location, *exp, *rhs);
	}
	if (accept<Token::NEqual>())
	{
		TRY(rhs, equalityExpression());
		return builder.create<mlir::rlc::NotEqualOp>(location, *exp, *rhs);
	}
	return std::move(*exp);
}

/**
 * orExpression : equalityExpression ('and' equalityExpression)*
 */
Expected<mlir::Value> Parser::andExpression()
{
	TRY(exp, equalityExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::KeywordAnd>())
	{
		TRY(rhs, andExpression());
		return builder.create<mlir::rlc::AndOp>(location, unkType(), *exp, *rhs);
	}
	return std::move(*exp);
}

/**
 * orExpression : andExpression ('or' andExpression)*
 */
Expected<mlir::Value> Parser::orExpression()
{
	TRY(exp, andExpression());

	auto location = getCurrentSourcePos();
	if (accept<Token::KeywordOr>())
	{
		TRY(rhs, orExpression());
		return builder.create<mlir::rlc::OrOp>(location, unkType(), *exp, *rhs);
	}
	return std::move(*exp);
}

Expected<mlir::Value> Parser::expression() { return assignmentExpression(); }

/**
 * assigmentExpression : orExpression | orExpression "=" assigmentExpression
 */
Expected<mlir::Value> Parser::assignmentExpression()
{
	TRY(leftHand, orExpression());

	auto location = getCurrentSourcePos();
	if (!accept<Token::Equal>())
		return std::move(*leftHand);

	TRY(rightHand, assignmentExpression());
	return builder.create<mlir::rlc::AssignOp>(
			location, unkType(), *leftHand, *rightHand);
}

/**
 * EntityField : TypeUse Identifier
 */
llvm::Expected<std::pair<std::string, mlir::rlc::ScalarUseType>>
Parser::entityField()
{
	TRY(type, singleTypeUse());
	EXPECT(Token::Identifier);
	return std::pair{ lIdent, *type };
}

/**
 * EntityDeclaration : Ent[templateArguments] Identifier Colons Newline Indent
 * (entityField Newline)* Deindent
 */
llvm::Expected<mlir::rlc::EntityDeclaration> Parser::entityDeclaration()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordEntity);
	SmallVector<mlir::Type, 3> templateParameters;
	if (current == Token::LAng)
	{
		TRY(parameters, templateArguments());
		for (auto type : std::move(*parameters))
			templateParameters.push_back(type);
	}

	EXPECT(Token::Identifier);
	string name = lIdent;
	EXPECT(Token::Colons);
	EXPECT(Token::Newline);
	EXPECT(Token::Indent);
	SmallVector<mlir::Type, 3> fieldTypes;
	SmallVector<mlir::Attribute, 3> fieldNames;

	while (!accept<Token::Deindent>())
	{
		TRY(field, entityField());
		fieldTypes.emplace_back(field->second);
		fieldNames.emplace_back(builder.getStringAttr(field->first));
		EXPECT(Token::Newline);
	}

	return builder.create<mlir::rlc::EntityDeclaration>(
			location,
			unkType(),
			builder.getStringAttr(name),
			builder.getTypeArrayAttr(fieldTypes),
			builder.getArrayAttr(fieldNames),
			builder.getTypeArrayAttr(templateParameters));
}

/**
 * expressionStatement : expression '\n'
 */
llvm::Expected<mlir::rlc::ExpressionStatement> Parser::expressionStatement()
{
	auto location = getCurrentSourcePos();
	auto expStatement = builder.create<mlir::rlc::ExpressionStatement>(location);

	auto pos = builder.saveInsertionPoint();
	builder.createBlock(&expStatement.getBody());
	TRY(exp, expression());
	builder.create<mlir::rlc::Yield>(location);
	builder.restoreInsertionPoint(pos);

	EXPECT(Token::Newline);

	return expStatement;
}

/**
 * prerequsitites: "req"" expression [Indent (expression* \n) Deindent ] \n
 */
llvm::Expected<bool> Parser::requirementList()
{
	auto location = getCurrentSourcePos();
	llvm::SmallVector<mlir::Value, 4> values;

	while (accept<Token::KeywordReq>())
	{
		TRY(rest, expression());
		values.push_back(*rest);
		EXPECT(Token::Newline);
	}

	builder.create<mlir::rlc::Yield>(location, values);
	return true;
}

/**
 * actionStatement : actionDeclaration '\n'
 */
llvm::Expected<mlir::rlc::ActionStatement> Parser::actionStatement()
{
	TRY(action, actionDeclaration());
	EXPECT(Token::Newline);
	auto pos = builder.saveInsertionPoint();

	llvm::SmallVector<std::string, 3> argNames;
	for (auto name : action->getArgNames())
		argNames.push_back(name.cast<mlir::StringAttr>().str());

	auto op = builder.create<mlir::rlc::ActionStatement>(
			action->getLoc(),
			action->getArgumentTypes(),
			action->getUnmangledName(),
			argNames);

	llvm::SmallVector<mlir::Location> locs;
	for (size_t i = 0; i < action->getArgumentTypes().size(); i++)
		locs.push_back(op->getLoc());

	builder.createBlock(
			&op.getPrecondition(), {}, action->getArgumentTypes(), locs);
	TRY(list, requirementList());
	action->erase();

	builder.restoreInsertionPoint(pos);
	return op;
}

/**
 * ifStatement : if expression ':\nindent statementList [else ':\n'
 * statementList ]
 */
llvm::Expected<mlir::rlc::IfStatement> Parser::ifStatement()
{
	auto location = getCurrentSourcePos();
	auto expStatement = builder.create<mlir::rlc::IfStatement>(location);
	auto pos = builder.saveInsertionPoint();

	EXPECT(Token::KeywordIf);
	builder.createBlock(&expStatement.getCondition());
	TRY(exp, expression());
	builder.create<mlir::rlc::Yield>(location, mlir::ValueRange(*exp));

	EXPECT(Token::Colons);
	EXPECT(Token::Newline);

	builder.createBlock(&expStatement.getTrueBranch());
	TRY(tBranch, statementList());
	emitYieldIfNeeded(location);

	builder.createBlock(&expStatement.getElseBranch());
	if (!accept<Token::KeywordElse>())
	{
		builder.create<mlir::rlc::Yield>(location);
		builder.restoreInsertionPoint(pos);
		return expStatement;
	}

	EXPECT(Token::Colons);
	EXPECT(Token::Newline);
	TRY(fBranch, statementList());

	emitYieldIfNeeded(location);
	builder.restoreInsertionPoint(pos);
	return expStatement;
}

/**
 * forFieldStatment: `for` ident `of` expression ':\n' statementList
 */
Expected<mlir::rlc::ForFieldStatement> Parser::forFieldStatement()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordFor);

	EXPECT(Token::Identifier);
	auto name = lIdent;

	auto expStatement =
			builder.create<mlir::rlc::ForFieldStatement>(location, name);

	EXPECT(Token::KeywordOf);

	auto pos = builder.saveInsertionPoint();
	builder.createBlock(&expStatement.getCondition());
	TRY(exp, expression());

	builder.create<mlir::rlc::Yield>(location, mlir::ValueRange({ *exp }));
	EXPECT(Token::Colons);
	EXPECT(Token::Newline);

	builder.createBlock(
			&expStatement.getBody(),
			expStatement.getBody().end(),
			mlir::TypeRange({ mlir::rlc::TemplateParameterType::get(
					builder.getContext(),
					(Twine("implicit_template_") + Twine(currentTemplateTypeIndex++))
							.str(),
					nullptr) }),
			{ location });
	TRY(statLis, statementList());

	emitYieldIfNeeded(location);
	builder.restoreInsertionPoint(pos);
	return expStatement;
}

/**
 * whileStatement : While exp ':\n' statementList
 */
Expected<mlir::rlc::WhileStatement> Parser::whileStatement()
{
	auto location = getCurrentSourcePos();
	EXPECT(Token::KeywordWhile);

	auto expStatement = builder.create<mlir::rlc::WhileStatement>(location);
	auto pos = builder.saveInsertionPoint();
	builder.createBlock(&expStatement.getCondition());

	TRY(exp, expression());
	builder.create<mlir::rlc::Yield>(location, mlir::ValueRange({ *exp }));
	EXPECT(Token::Colons);
	EXPECT(Token::Newline);

	builder.createBlock(&expStatement.getBody());
	TRY(statLis, statementList());
	emitYieldIfNeeded(location);

	builder.restoreInsertionPoint(pos);
	return expStatement;
}

/**
 * returnStatement : return [expression] '\n'
 */
Expected<mlir::rlc::ReturnStatement> Parser::returnStatement()
{
	auto location = getCurrentSourcePos();
	auto expStatement =
			builder.create<mlir::rlc::ReturnStatement>(location, unkType());

	auto pos = builder.saveInsertionPoint();
	builder.createBlock(&expStatement.getBody());

	EXPECT(Token::KeywordReturn);
	if (accept(Token::Newline))
	{
		builder.create<mlir::rlc::Yield>(location, mlir::ValueRange());
		builder.restoreInsertionPoint(pos);
		return expStatement;
	}

	TRY(exp, expression());
	EXPECT(Token::Newline);
	builder.create<mlir::rlc::Yield>(location, mlir::ValueRange({ *exp }));
	builder.restoreInsertionPoint(pos);
	return expStatement;
}

Expected<mlir::Operation*> Parser::statement()
{
	if (current == Token::KeywordAction)
		return actionStatement();

	if (current == Token::KeywordIf)
		return ifStatement();

	if (current == Token::KeywordReturn)
		return returnStatement();

	if (current == Token::KeywordWhile)
		return whileStatement();

	if (current == Token::KeywordFor)
		return forFieldStatement();

	if (current == Token::KeywordLet)
		return declarationStatement();

	if (current == Token::KeywordFree)
		return builtinFree();

	if (current == Token::KeywordDestroy)
		return builtinDestroy();

	return expressionStatement();
}

void Parser::emitYieldIfNeeded(mlir::Location loc)
{
	if (not builder.getBlock()->empty() and
			builder.getBlock()->back().hasTrait<mlir::OpTrait::IsTerminator>())
		return;

	builder.create<mlir::rlc::Yield>(loc);
}

/**
 * declarationStatement : 'let' identifier ['=' expression | ':' type_use ]
 */
Expected<mlir::rlc::DeclarationStatement> Parser::declarationStatement()
{
	EXPECT(Token::KeywordLet);
	EXPECT(Token::Identifier);
	auto location = getCurrentSourcePos();
	auto name = lIdent;

	auto expStatement = builder.create<mlir::rlc::DeclarationStatement>(
			location, unkType(), name);

	auto pos = builder.saveInsertionPoint();
	builder.createBlock(&expStatement.getBody());

	if (accept<Token::Equal>())
	{
		TRY(exp, expression());
		EXPECT(Token::Newline);

		builder.create<mlir::rlc::Yield>(location, mlir::ValueRange({ *exp }));
		builder.restoreInsertionPoint(pos);
		return expStatement;
	}

	EXPECT(Token::Colons);
	TRY(use, singleTypeUse());
	EXPECT(Token::Newline);

	auto exp = builder.create<mlir::rlc::ConstructOp>(location, *use);
	builder.create<mlir::rlc::Yield>(location, mlir::ValueRange({ exp }));

	builder.restoreInsertionPoint(pos);
	return expStatement;
}

/**
 * statmentList : indent (statement)* deindent
 */
Expected<bool> Parser::statementList()
{
	EXPECT(Token::Indent);
	while (!accept<Token::Deindent>())
	{
		TRY(s, statement());
	}

	return true;
}

/**
 * functionTypeUse : [singleTypeUse ("->" singleTypeUse )*]
 */
Expected<mlir::rlc::FunctionUseType> Parser::functionTypeUse()
{
	SmallVector<mlir::Type, 2> tpUse;

	TRY(singleTp, singleTypeUse());
	tpUse.emplace_back(std::move(*singleTp));
	while (accept<Token::Arrow>())
	{
		TRY(singleTp, singleTypeUse());
		tpUse.emplace_back(std::move(*singleTp));
	}

	return mlir::rlc::FunctionUseType::get(ctx, tpUse);
}

/**
 * singleTypeUse : "(" functionType ")" | identifier["<"singleTypeUse (,
 * singleTypeUse)* ">"]["["int64"]"] | "OwningPtr<" singleTypeUse ">"
 */
Expected<mlir::rlc::ScalarUseType> Parser::singleTypeUse()
{
	if (accept<Token::LPar>())
	{
		TRY(fType, functionTypeUse());
		EXPECT(Token::RPar);
		return mlir::rlc::ScalarUseType::get(ctx, *fType, "", 0, {});
	}

	if (accept<Token::KeywordOwningPtr>())
	{
		EXPECT(Token::LAng);
		TRY(subType, singleTypeUse());
		EXPECT(Token::RAng);
		return mlir::rlc::ScalarUseType::get(
				ctx,
				mlir::rlc::OwningPtrType::get(subType->getContext(), *subType),
				"",
				0,
				{});
	}

	EXPECT(Token::Identifier);
	auto nm = lIdent;
	int64_t arraySize = 0;
	llvm::SmallVector<mlir::Type, 2> templateParametersTypes;
	if (accept<Token::LAng>())
	{
		do
		{
			TRY(templateParameter, singleTypeUse());
			templateParametersTypes.push_back(*templateParameter);
		} while (accept<Token::Comma>());
		EXPECT(Token::RAng);
	}

	if (accept<Token::LSquare>())
	{
		EXPECT(Token::Int64);
		arraySize = lInt64;
		EXPECT(Token::RSquare);
	}
	return mlir::rlc::ScalarUseType::get(
			ctx, nullptr, nm, arraySize, templateParametersTypes);
}

Expected<std::pair<std::string, mlir::rlc::ScalarUseType>>
Parser::argDeclaration()
{
	TRY(tp, singleTypeUse());
	EXPECT(Token::Identifier);
	auto parName = lIdent;
	return std::pair{ parName, *tp };
}

/**
 * functionDefinition : "(" [argDeclaration ("," argDeclaration)*] ")"
 */
Expected<llvm::SmallVector<std::pair<std::string, mlir::rlc::ScalarUseType>, 3>>
Parser::functionArguments()
{
	EXPECT(Token::LPar);

	llvm::SmallVector<std::pair<std::string, mlir::rlc::ScalarUseType>, 3> args;

	if (current != Token::RPar)
	{
		do
		{
			TRY(arg, argDeclaration());
			args.emplace_back(std::move(*arg));
		} while (accept<Token::Comma>());
	}

	EXPECT(Token::RPar);
	return args;
}

/**
 * templateArguments: "<" ident ident ("," ident ident?)* ">"
 */
Expected<llvm::SmallVector<mlir::rlc::UncheckedTemplateParameterType, 2>>
Parser::templateArguments()
{
	llvm::SmallVector<mlir::rlc::UncheckedTemplateParameterType, 2> toReturn;
	EXPECT(Token::LAng);

	do
	{
		EXPECT(Token::Identifier);
		auto first = lIdent;
		if (accept<Token::Identifier>())
		{
			auto second = lIdent;
			toReturn.push_back(mlir::rlc::UncheckedTemplateParameterType::get(
					builder.getContext(), second, builder.getStringAttr(first)));
		}
		else
		{
			toReturn.push_back(mlir::rlc::UncheckedTemplateParameterType::get(
					builder.getContext(), first, builder.getStringAttr("")));
		}

	} while (accept<Token::Comma>());

	EXPECT(Token::RAng);
	return toReturn;
}

Expected<Parser::FunctionDeclarationResult> Parser::externFunctionDeclaration()
{
	EXPECT(Token::KeywordExtern);
	TRY(result, functionDeclaration());
	EXPECT(Token::Newline);
	return std::move(*result);
}

/**
 * functionDeclaration : "fun" templateArguments identifier "("
 * [argDeclaration
 * ("," argDeclaration)*] ")" ["->" singleTypeUse]
 */
Expected<Parser::FunctionDeclarationResult> Parser::functionDeclaration(
		bool templateFunction)
{
	auto location = getCurrentSourcePos();
	auto pos = builder.saveInsertionPoint();

	EXPECT(Token::KeywordFun);

	llvm::SmallVector<mlir::Type, 2> templateParameters;
	if (templateFunction and current == Token::LAng)
	{
		TRY(parameters, templateArguments());
		for (auto type : std::move(*parameters))
			templateParameters.push_back(type);
	}

	EXPECT(Token::Identifier);
	auto nm = lIdent;
	TRY(args, functionArguments());
	llvm::SmallVector<mlir::Type> argTypes;
	llvm::SmallVector<llvm::StringRef> argName;
	llvm::SmallVector<mlir::Location> argLocs;

	for (auto& arg : *args)
	{
		argTypes.push_back(arg.second);
		argName.push_back(arg.first);
		argLocs.push_back(location);
	}

	mlir::Type retType = mlir::rlc::VoidType::get(ctx);
	if (accept<Token::Arrow>())
	{
		TRY(actualRetType, singleTypeUse());
		retType = *actualRetType;
	}

	auto fun = builder.create<mlir::rlc::FunctionOp>(
			location,
			mlir::FunctionType::get(ctx, argTypes, { retType }),
			builder.getStringAttr(nm),
			builder.getStrArrayAttr(argName),
			builder.getTypeArrayAttr(templateParameters));

	builder.restoreInsertionPoint(pos);
	return FunctionDeclarationResult{ fun, argLocs };
}

/**
 * functionDefinition : functionDeclaration ":\n" statementList
 */
Expected<mlir::rlc::FunctionOp> Parser::functionDefinition()
{
	TRY(result, functionDeclaration());
	auto fun = result->op;
	auto location = getCurrentSourcePos();
	auto pos = builder.saveInsertionPoint();

	EXPECT(Token::Colons);
	EXPECT(Token::Newline);

	builder.createBlock(
			&fun.getPrecondition(), {}, fun.getArgumentTypes(), result->argLocs);
	TRY(list, requirementList());

	builder.createBlock(
			&fun.getBody(), {}, fun.getArgumentTypes(), result->argLocs);
	TRY(body, statementList());

	emitYieldIfNeeded(location);
	builder.restoreInsertionPoint(pos);
	return fun;
}

/**
 * actionDeclaration : "act" identifier "(" [argDeclaration (","
 * argDeclaration)*] ")"
 */
Expected<mlir::rlc::ActionFunction> Parser::actionDeclaration()
{
	auto location = getCurrentSourcePos();

	EXPECT(Token::KeywordAction);
	EXPECT(Token::Identifier);
	auto nm = lIdent;

	TRY(args, functionArguments());
	llvm::SmallVector<mlir::Type> argTypes;
	llvm::SmallVector<llvm::StringRef> argName;

	for (auto& arg : *args)
	{
		argTypes.push_back(arg.second);
		argName.push_back(arg.first);
	}

	auto retType = mlir::rlc::ScalarUseType::get(ctx, nullptr, "Void", 0, {});
	return builder.create<mlir::rlc::ActionFunction>(
			location,
			mlir::FunctionType::get(ctx, argTypes, { retType }),
			mlir::rlc::UnknownType::get(builder.getContext()),
			mlir::TypeRange(),
			builder.getStringAttr(nm),
			builder.getStrArrayAttr(argName));
}

/**
 * actionDefinition : actionDeclaration ":\n" statementList
 */
Expected<mlir::rlc::ActionFunction> Parser::actionDefinition()
{
	TRY(decl, actionDeclaration());
	auto location = getCurrentSourcePos();
	EXPECT(Token::Colons);
	EXPECT(Token::Newline);

	auto pos = builder.saveInsertionPoint();
	llvm::SmallVector<mlir::Location> locs;
	for (size_t i = 0; i < decl->getFunctionType().getInputs().size(); i++)
		locs.push_back(decl->getLoc());

	builder.createBlock(
			&decl->getPrecondition(), {}, decl->getFunctionType().getInputs(), locs);
	TRY(list, requirementList());

	auto block = builder.createBlock(
			&decl->getBody(), {}, decl->getFunctionType().getInputs(), locs);
	builder.setInsertionPoint(block, block->begin());
	TRY(body, statementList());
	emitYieldIfNeeded(location);
	builder.restoreInsertionPoint(pos);
	return decl;
}

/**
 * traitDefinition: trait "<" ident ">:\n" indent (functionDeclaration "\n")*
 * deindent
 */
Expected<mlir::rlc::UncheckedTraitDefinition> Parser::traitDefinition()
{
	auto location = getCurrentSourcePos();
	auto pos = builder.saveInsertionPoint();
	EXPECT(Token::KeywordTrait);
	EXPECT(Token::LAng);
	EXPECT(Token::Identifier);
	auto templateParameter = lIdent;
	EXPECT(Token::RAng);
	EXPECT(Token::Identifier);
	auto traitName = lIdent;
	auto trait = builder.create<mlir::rlc::UncheckedTraitDefinition>(
			location, traitName, templateParameter);
	builder.createBlock(&trait.getBody());
	builder.setInsertionPoint(
			&trait.getBody().front(), trait.getBody().front().begin());

	EXPECT(Token::Colons);
	EXPECT(Token::Newline);
	EXPECT(Token::Indent);
	while (not accept<Token::Deindent>())
	{
		TRY(fun, functionDeclaration());
		EXPECT(Token::Newline);
	}

	builder.restoreInsertionPoint(pos);
	return trait;
}

Expected<mlir::ModuleOp> Parser::system(mlir::ModuleOp destination)
{
	auto location = getCurrentSourcePos();
	std::string name = "unknown";
	if (accept<Token::KeywordSystem>())
	{
		EXPECT(Token::Identifier);
		name = lIdent;
		EXPECT(Token::Newline);
	}

	auto module = destination == nullptr
										? mlir::ModuleOp::create(location, llvm::StringRef(name))
										: destination;
	builder.setInsertionPointToStart(
			&*module.getBodyRegion().getBlocks().begin());

	while (current != Token::End)
	{
		while (accept<Token::Newline>() or accept<Token::Indent>() or
					 accept<Token::Deindent>())
			;

		if (current == Token::KeywordExtern)
		{
			TRY(f, externFunctionDeclaration());
			continue;
		}

		if (current == Token::KeywordAction)
		{
			TRY(f, actionDefinition());
			continue;
		}

		if (current == Token::KeywordFun)
		{
			TRY(f, functionDefinition());
			continue;
		}

		if (current == Token::KeywordEntity)
		{
			TRY(f, entityDeclaration());
			continue;
		}

		if (accept<Token::KeywordImport>())
		{
			EXPECT(Token::Identifier);

			std::string subFile = lIdent;
			while (accept<Token::Dot>())
			{
				subFile.append("/");
				EXPECT(Token::Identifier);
				subFile.append(lIdent);
			}
			EXPECT(Token::Newline);
			importedFiles.push_back(subFile + ".rl");
			continue;
		}

		if (current == Token::KeywordTrait)
		{
			TRY(f, traitDefinition());
			continue;
		}
		auto location = getCurrentSourcePos();
		return make_error<RlcError>(
				"Expected function, action or entity declaration",
				RlcErrorCategory::errorCode(RlcErrorCode::unexpectedToken),
				location);
	}
	return module;
}
