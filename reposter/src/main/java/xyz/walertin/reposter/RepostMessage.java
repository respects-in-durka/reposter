package xyz.walertin.reposter;

import com.fasterxml.jackson.annotation.JsonProperty;
import lombok.Data;

import java.util.List;

@Data
public class RepostMessage {
    @JsonProperty("chat_name")
    private String chatName;
    @JsonProperty("chat_icon")
    private String chatIcon;
    private String text;
    private List<String> files;
}
