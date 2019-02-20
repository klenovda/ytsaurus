package internal

type Verb string

const (
	VerbCreate Verb = "create"
	VerbExists Verb = "exists"
	VerbRemove Verb = "remove"
	VerbGet    Verb = "get"
	VerbSet    Verb = "set"
	VerbList   Verb = "list"
	VerbCopy   Verb = "copy"
	VerbMove   Verb = "move"
	VerbLink   Verb = "link"

	VerbWriteFile        Verb = "write_file"
	VerbReadFile         Verb = "read_file"
	VerbPutFileToCache   Verb = "put_file_to_cache"
	VerbGetFileFromCache Verb = "get_file_from_cache"

	VerbWriteTable Verb = "write_table"
	VerbReadTable  Verb = "read_table"

	VerbStartOperation Verb = "start_operation"
	VerbGetOperation   Verb = "get_operation"

	VerbStartTransaction  Verb = "start_transaction"
	VerbPingTransaction   Verb = "ping_transaction"
	VerbAbortTransaction  Verb = "abort_transaction"
	VerbCommitTransaction Verb = "commit_transaction"

	VerbAddMember    Verb = "add_member"
	VerbRemoveMember Verb = "remove_member"

	VerbLock   Verb = "lock"
	VerbUnlock Verb = "unlock"
)

func (v Verb) hasInput() bool {
	switch v {
	case VerbSet, VerbWriteFile, VerbWriteTable:
		return true
	}

	return false
}

func (v Verb) IsHeavy() bool {
	switch v {
	case VerbReadFile, VerbWriteFile, VerbReadTable, VerbWriteTable:
		return true
	}

	return false
}

func (v Verb) volatile() bool {
	switch v {
	case VerbGet, VerbList, VerbExists, VerbReadFile, VerbReadTable, VerbGetOperation, VerbGetFileFromCache:
		return false
	}

	return true
}

func (v Verb) String() string {
	return string(v)
}

func (v Verb) HttpMethod() string {
	if v.hasInput() {
		return "PUT"
	} else if v.volatile() {
		return "POST"
	} else {
		return "GET"
	}
}
