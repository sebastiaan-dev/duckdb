#pragma once

namespace duckdb {
namespace variant {

template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertMapToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count, optional_ptr<const SelectionVector> selvec, optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	// We get the vectors which map to the logical segments of the VARIANT type
	auto keys_offset_data = OffsetData::GetKeys(result.offsets);
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);

	auto &keys = MapVector::GetKeys(source.vec);
	auto child_size = ListVector::GetListSize(source.vec);

	Vector key_strings(LogicalType::VARCHAR, child_size);
	if (keys.GetType().id() == LogicalTypeId::VARCHAR) {
		key_strings.Reference(keys);
	} else {
		VectorOperations::DefaultTryCast(keys, key_strings, child_size, nullptr);
	}

	VectorIterator<string_t> keys_iterator(key_strings, child_size);

	// Unified view of the source vector (list), we do this because we don't know if the list vector is a flat vector,
	// in which case we could access it directly, or if it is another form like dictionary.
	auto &source_format = source.source_format;
	auto &source_validity = source_format.validity;
	// We get the entries from the uppermost vector, so that we can index into the vector(s) in the child.
	auto source_data = source_format.GetData<list_entry_t>(source_format);

	// Loop to find out which rows in the vector are valid, i.e. NULL or defined.
	auto &variant = result.variant;
	idx_t list_size = 0;
	for (idx_t i = 0; i < count; i++) {
		// We have to fetch the physical location of the row, based on the logical index i.
		const auto index = source[i];
		if (!source_validity.RowIsValid(index)) {
			continue;
		}
		// Fetch the actual entry associated with the logical id.
		auto &entry = source_data[index];
		// entries are defined as an offset + a length, so all lengths combined give the total amount of elements in
		// the list overall all rows in the vector (and of course, when an entry is invalid there are no elements, so length is not increased)
		list_size += entry.length;
	}

	// create 3 selection vectors, which maintains mappings for ease of implementation:
	// (1) new_selection - this maintains a mapping from the location of the child element to the row location in the resulting vector
	// (global to global)
	// (2) non_null_selection -
	// (3) children_selection - this maintains a mapping from the location of the child element to the which index it has in its own row
	// (global to non-global index value)
	ContainerSelectionVectors sel(list_size);
	for (idx_t i = 0; i < count; i++) {
		// Fetch source index
		const auto index = source[i];
		// Below we take care of recursive calls. At the root we have the identity where the index of
		// the source row = the index of the result row. But when we execute a recursive call we need to know where to place
		// source values into the global resulting vector. So selvec acts as a routing table, defined separately for each child.
		// These children can then use this routing table to build a new routing table for their children and so forth.
		const auto result_index = selvec ? selvec->get_index(i) : i;

		auto &blob_offset = blob_offset_data[result_index];

		auto &children_list_entry = variant.children_data[result_index];
		if (source_validity.RowIsValid(index)) {
			auto &entry = source_data[index];
			// Write the metadata into the values of the result variant, with the root object set to OBJECT
			WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, values_index_selvec,
			                                 i, VariantLogicalType::OBJECT);
			// Write into the blob data of the result variant.
			WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset, entry.length,
			                               children_offset_data[result_index]);

			for (idx_t child_idx = 0; child_idx < entry.length; child_idx++) {
				// Apply the recursion to the new selection vector
				sel.new_selection.set_index(sel.count + child_idx, result_index);
				if (WRITE_DATA) {
					idx_t children_index = children_list_entry.offset + children_offset_data[result_index];
					sel.children_selection.set_index(child_idx + sel.count, children_index + child_idx);

					auto &keys_list_entry = variant.keys_data[result_index];
					auto keys_offset = keys_list_entry.offset + keys_offset_data[result_index];
					auto key_index = keys_offset_data[result_index] + child_idx;
					auto dictionary_index = result.GetOrCreateIndex(keys_iterator[entry.offset + child_idx].GetValue());

					variant.keys_index_data[children_index + child_idx] = NumericCast<uint32_t>(key_index);
					result.keys_selvec.set_index(keys_offset + child_idx, dictionary_index);
				}
				sel.non_null_selection.set_index(sel.count + child_idx, entry.offset + child_idx);
			}
			// Update the offset for the next child.
			keys_offset_data[result_index] += entry.length;
			children_offset_data[result_index] += entry.length;
			sel.count += entry.length;
		} else if (!IGNORE_NULLS) {
			HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, values_index_selvec, i,
			                              is_root);
		}
	}
	//! Now write the child vector of the list (for all rows)
	auto &values = MapVector::GetValues(source.vec);

	// Why do we distinguish here?
	if (sel.count != list_size) {
		Vector sliced_values(values, sel.non_null_selection, sel.count);
		ToVariantSourceData child_source_data(sliced_values, sel.count);
		// Recurse into the children
		return ConvertToVariant<WRITE_DATA, false>(child_source_data, result, sel.count, &sel.new_selection,
		                                           &sel.children_selection, false);
	} else {
		//! All rows are valid, no need to slice the child
		ToVariantSourceData child_source_data(values, child_size, sel.non_null_selection);
		// Recurse into the children
		return ConvertToVariant<WRITE_DATA, false>(child_source_data, result, sel.count, &sel.new_selection,
		                                           &sel.children_selection, false);
	}
}

} // namespace variant
} // namespace duckdb