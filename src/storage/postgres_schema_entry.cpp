#include "storage/postgres_schema_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "storage/postgres_table_entry.hpp"
#include "storage/postgres_transaction.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

PostgresSchemaEntry::PostgresSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this), indexes(*this), types(*this) {
}

PostgresSchemaEntry::PostgresSchemaEntry(Catalog &catalog, CreateSchemaInfo &info,
                                         unique_ptr<PostgresResultSlice> tables, unique_ptr<PostgresResultSlice> enums,
                                         unique_ptr<PostgresResultSlice> composite_types,
                                         unique_ptr<PostgresResultSlice> indexes)
    : SchemaCatalogEntry(catalog, info), tables(*this, std::move(tables)), indexes(*this, std::move(indexes)),
      types(*this, std::move(enums), std::move(composite_types)) {
}

bool PostgresSchemaEntry::SchemaIsInternal(const string &name) {
	if (name == "information_schema" || StringUtil::StartsWith(name, "pg_")) {
		return true;
	}
	return false;
}

PostgresTransaction &GetPostgresTransaction(CatalogTransaction transaction) {
	if (!transaction.transaction) {
		throw InternalException("No transaction!?");
	}
	return transaction.transaction->Cast<PostgresTransaction>();
}

void PostgresSchemaEntry::TryDropEntry(ClientContext &context, CatalogType catalog_type, const string &name) {
	DropInfo info;
	info.type = catalog_type;
	info.SetName(Identifier(name));
	info.cascade = false;
	info.if_not_found = OnEntryNotFound::RETURN_NULL;
	DropEntry(context, info);
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                            BoundCreateTableInfo &info) {
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	auto &base_info = info.Base();
	string table_name = base_info.GetTableName().GetIdentifierName();
	if (base_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// CREATE OR REPLACE - drop any existing entries first (if any)
		TryDropEntry(transaction.GetContext(), CatalogType::TABLE_ENTRY, table_name);
	}
	return tables.CreateTable(postgres_transaction, info);
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                               CreateFunctionInfo &info) {
	throw BinderException("Postgres databases do not support creating functions");
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                            TableCatalogEntry &table) {
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	return indexes.CreateIndex(postgres_transaction, info, table);
}

string PGGetCreateViewSQL(PostgresSchemaEntry &schema, CreateViewInfo &info, bool or_replace) {
	string sql;
	sql = or_replace ? "CREATE OR REPLACE VIEW " : "CREATE VIEW ";
	sql += PostgresUtils::QuotePostgresIdentifier(schema.name.GetIdentifierName()) + ".";
	sql += PostgresUtils::QuotePostgresIdentifier(info.GetViewName().GetIdentifierName());
	sql += " ";
	if (!info.aliases.empty()) {
		sql += "(";
		for (idx_t i = 0; i < info.aliases.size(); i++) {
			if (i > 0) {
				sql += ", ";
			}
			auto &alias = info.aliases[i];
			sql += PostgresUtils::QuotePostgresIdentifier(alias.GetIdentifierName());
		}
		sql += ") ";
	}
	sql += "AS ";
	sql += info.query->ToString();
	return sql;
}

//! Replaces the view in place, which keeps its OID so that dependent views stay bound.
//! Returns false if Postgres rejects the new definition as incompatible with the existing columns.
static bool TryReplaceViewInPlace(PostgresSchemaEntry &schema, PostgresTransaction &transaction, CreateViewInfo &info) {
	// a failed statement aborts the whole Postgres transaction, so the attempt needs its own savepoint
	transaction.Query("SAVEPOINT duckdb_replace_view");
	string error_message;
	string sqlstate;
	auto result = transaction.TryQuery(PGGetCreateViewSQL(schema, info, true), error_message, sqlstate);
	if (result) {
		transaction.Query("RELEASE SAVEPOINT duckdb_replace_view");
		return true;
	}
	transaction.Query("ROLLBACK TO SAVEPOINT duckdb_replace_view; RELEASE SAVEPOINT duckdb_replace_view");
	// 42P16 (invalid_table_definition): the new query renames, retypes or removes existing columns
	if (sqlstate != "42P16") {
		throw std::runtime_error(error_message);
	}
	return false;
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	if (info.sql.empty()) {
		throw BinderException("Cannot create view in Postgres that originated from an empty SQL statement");
	}
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	auto view_name = info.GetViewName().GetIdentifierName();
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT ||
	    info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		auto current_entry = GetEntry(transaction, CatalogType::VIEW_ENTRY, info.GetViewName());
		if (current_entry) {
			if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
				return current_entry;
			}
			if (TryReplaceViewInPlace(*this, postgres_transaction, info)) {
				return tables.ReloadEntry(postgres_transaction, view_name);
			}
			// the new definition is incompatible - drop the existing view and create it again
			TryDropEntry(transaction.GetContext(), CatalogType::VIEW_ENTRY, view_name);
		}
	}
	postgres_transaction.Query(PGGetCreateViewSQL(*this, info, false));
	return tables.ReloadEntry(postgres_transaction, view_name);
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	auto type_name = info.GetTypeName();
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		// CREATE OR REPLACE - drop any existing entries first (if any)
		TryDropEntry(transaction.GetContext(), CatalogType::TYPE_ENTRY, info.GetTypeName().GetIdentifierName());
	}
	return types.CreateType(postgres_transaction, info);
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                               CreateSequenceInfo &info) {
	throw BinderException("Postgres databases do not support creating sequences");
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                    CreateTableFunctionInfo &info) {
	throw BinderException("Postgres databases do not support creating table functions");
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                   CreateCopyFunctionInfo &info) {
	throw BinderException("Postgres databases do not support creating copy functions");
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                     CreatePragmaFunctionInfo &info) {
	throw BinderException("Postgres databases do not support creating pragma functions");
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                                CreateCollationInfo &info) {
	throw BinderException("Postgres databases do not support creating collations");
}

void PostgresSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		throw BinderException("Only altering tables is supported for now");
	}
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	auto &alter = info.Cast<AlterTableInfo>();
	tables.AlterTable(transaction.GetContext(), postgres_transaction, alter);
}

bool CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::INDEX_ENTRY:
	case CatalogType::TABLE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return true;
	default:
		return false;
	}
}

void PostgresSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (!CatalogTypeIsSupported(type)) {
		return;
	}
	auto &postgres_transaction = PostgresTransaction::Get(context, catalog);
	GetCatalogSet(type).Scan(context, postgres_transaction, callback);
}
void PostgresSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scan without context not supported");
}

void PostgresSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	info.SetQualifiedName(QualifiedName(info.GetQualifiedName().Catalog(), name, info.GetQualifiedName().Name()));
	auto &postgres_transaction = PostgresTransaction::Get(context, catalog);
	GetCatalogSet(info.type).DropEntry(postgres_transaction, info);
}

optional_ptr<CatalogEntry> PostgresSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	auto catalog_type = lookup_info.GetCatalogType();
	if (!CatalogTypeIsSupported(catalog_type)) {
		return nullptr;
	}
	auto &postgres_transaction = GetPostgresTransaction(transaction);
	return GetCatalogSet(catalog_type)
	    .GetEntry(transaction.GetContext(), postgres_transaction, lookup_info.GetEntryName());
}

PostgresCatalogSet &PostgresSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return tables;
	case CatalogType::INDEX_ENTRY:
		return indexes;
	case CatalogType::TYPE_ENTRY:
		return types;
	default:
		throw InternalException("Type not supported for GetCatalogSet");
	}
}

} // namespace duckdb
