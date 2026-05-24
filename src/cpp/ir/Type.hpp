#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Real generic type system — preserved in bytecode, no erasure.
// Declaration-site variance for list<T> (like Kotlin/C#).

enum class TypeKind : uint8_t {
	Int = 0,
	Str = 1,
	List = 2,
	Bool = 3,
	Long = 4,
	Float = 5,
	Double = 6,
	Class = 7,
	None = 8,
};

enum class Variance : uint8_t {
	Invariant = 0,  // list<T>     — read/write
	Covariant = 1,  // list<out T> — read-only
	Contravariant = 2, // list<in T>  — write-only
};

struct TypeNode {
	TypeKind kind;
	std::shared_ptr <TypeNode> elementType;
	std::string className;
	Variance variance = Variance::Invariant;  // meaningful for List

	TypeNode() : kind(TypeKind::Int) {}

	explicit TypeNode(TypeKind k) : kind(k) {}

	TypeNode(TypeKind k, std::shared_ptr <TypeNode> elem, Variance v = Variance::Invariant)
			: kind(k), elementType(std::move(elem)), variance(v) {}

	TypeNode(TypeKind k, const std::string &name)
			: kind(k), className(name) {}

	// Exact equality (not subtype check)
	bool operator==(const TypeNode &other) const {
		if (kind != other.kind) return false;
		if (kind == TypeKind::List)
			return variance == other.variance
			       && elementType && other.elementType
			       && *elementType == *other.elementType;
		if (kind == TypeKind::Class)
			return className == other.className;
		return true;
	}

	// A <: B ?
	static bool isSubtypeOf(const TypeNode &sub, const TypeNode &sup) {
		// Same type → trivially true
		if (sub == sup) return true;
		// Class inheritance (stub — no inheritance yet)
		// List variance
		if (sub.kind == TypeKind::List && sup.kind == TypeKind::List) {
			if (sub.variance == Variance::Covariant || sup.variance == Variance::Covariant) {
				// list<out Child> <: list<out Parent>  iff  Child <: Parent
				return isSubtypeOf(*sub.elementType, *sup.elementType);
			}
			if (sub.variance == Variance::Contravariant || sup.variance == Variance::Contravariant) {
				// list<in Parent> <: list<in Child>  iff  Child <: Parent  (flipped)
				return isSubtypeOf(*sup.elementType, *sub.elementType);
			}
		}
		return false;
	}

	void serialize(std::vector <uint8_t> &out) const {
		out.push_back(static_cast<uint8_t>(kind));
		if (kind == TypeKind::List) {
			out.push_back(static_cast<uint8_t>(variance));
			if (elementType) elementType->serialize(out);
		}
		if (kind == TypeKind::Class) {
			uint32_t len = (uint32_t) className.size();
			out.push_back((uint8_t)(len & 0xFF));
			out.push_back((uint8_t)((len >> 8) & 0xFF));
			out.push_back((uint8_t)((len >> 16) & 0xFF));
			out.push_back((uint8_t)((len >> 24) & 0xFF));
			for (char c: className) out.push_back((uint8_t) c);
		}
	}

	static TypeNode deserialize(const std::vector <uint8_t> &data, size_t &pos) {
		auto k = static_cast<TypeKind>(data[pos++]);
		if (k == TypeKind::List) {
			auto v = static_cast<Variance>(data[pos++]);
			auto elem = std::make_shared<TypeNode>(deserialize(data, pos));
			return TypeNode(k, elem, v);
		}
		if (k == TypeKind::Class) {
			uint32_t len = 0;
			for (int i = 0; i < 4; i++)
				len |= (uint32_t) data[pos++] << (i * 8);
			std::string name(data.begin() + pos, data.begin() + pos + len);
			pos += len;
			return TypeNode(k, name);
		}
		return TypeNode(k);
	}

	std::string toString() const {
		switch (kind) {
			case TypeKind::Int:
				return "int";
			case TypeKind::Str:
				return "str";
			case TypeKind::Bool:
				return "bool";
			case TypeKind::Long:
				return "long";
			case TypeKind::Float:
				return "float";
			case TypeKind::Double:
				return "double";
			case TypeKind::Class:
				return className;
			case TypeKind::None:
				return "None";
			case TypeKind::List: {
				std::string var;
				if (variance == Variance::Covariant) var = "out ";
				else if (variance == Variance::Contravariant) var = "in ";
				return "list<" + var + (elementType ? elementType->toString() : "?") + ">";
			}
		}
		return "?";
	}

	static TypeNode fromString(const std::string &s) {
		if (s == "int") return TypeNode(TypeKind::Int);
		if (s == "str") return TypeNode(TypeKind::Str);
		if (s == "bool") return TypeNode(TypeKind::Bool);
		if (s == "long") return TypeNode(TypeKind::Long);
		if (s == "float") return TypeNode(TypeKind::Float);
		if (s == "double") return TypeNode(TypeKind::Double);
		if (s == "None") return TypeNode(TypeKind::None);
		return TypeNode(TypeKind::Int);
	}
};
