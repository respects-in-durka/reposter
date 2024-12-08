package xyz.walertin.reposter;

import com.fasterxml.jackson.databind.ObjectMapper;
import lombok.SneakyThrows;
import org.springframework.amqp.core.Message;
import org.springframework.amqp.core.MessageListener;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.configurationprocessor.json.JSONObject;
import org.springframework.core.io.ByteArrayResource;
import org.springframework.core.io.Resource;
import org.springframework.http.*;
import org.springframework.stereotype.Component;
import org.springframework.util.LinkedMultiValueMap;
import org.springframework.util.MultiValueMap;
import org.springframework.web.client.RestTemplate;

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

@Component
public class TGMessageListener implements MessageListener {

    @Value("${reposter.webhook}")
    private String webhook;

    @SneakyThrows
    @Override
    public void onMessage(Message message) {
        ObjectMapper mapper = new ObjectMapper();
        RepostMessage repost = mapper.readValue(message.getBody(), RepostMessage.class);

        if (repost.getFiles() != null && !repost.getFiles().isEmpty()) {
            RestTemplate restTemplate = new RestTemplate();
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.MULTIPART_FORM_DATA);


            MultiValueMap<String, Object> body = new LinkedMultiValueMap<>();
            Map<String, String> payload = new HashMap<>();
            payload.put("content", repost.getText().isBlank() ? "[Original message unavailable]" : repost.getText());
            payload.put("username", repost.getChatName());
            payload.put("avatar_url", repost.getChatIcon());
            body.add("payload_json", payload);


            int fileIndex = 1;
            for (String fileUrl : repost.getFiles()) {
                ResponseEntity<Resource> exchange = restTemplate.exchange(fileUrl, HttpMethod.GET, null, Resource.class);
                if (!exchange.getStatusCode().is2xxSuccessful()) {
                    continue;
                }

                body.add(String.format("file%d", fileIndex++), new ByteArrayResource(Objects.requireNonNull(exchange.getBody()).getContentAsByteArray()){
                    @Override
                    public String getFilename() {
                        return fileUrl.split("/")[fileUrl.split("/").length - 1];
                    }
                });
            }

            HttpEntity<MultiValueMap<String, ?>> requestEntity = new HttpEntity<>(body, headers);
            try {
                restTemplate.postForEntity(webhook, requestEntity, String.class);
            } catch (Exception e) {
                headers = new HttpHeaders();
                headers.setContentType(MediaType.APPLICATION_JSON);

                JSONObject text = new JSONObject();
                text.put("content", repost.getText().isBlank() || repost.getText().isEmpty() ? "[Original entity too large]" : repost.getText());
                text.put("username", repost.getChatName());
                text.put("avatar_url", repost.getChatIcon());

                HttpEntity<String> request = new HttpEntity<>(text.toString(), headers);
                restTemplate.postForEntity(webhook, request, String.class);
            }
        } else {
            RestTemplate restTemplate = new RestTemplate();
            HttpHeaders headers = new HttpHeaders();
            headers.setContentType(MediaType.APPLICATION_JSON);

            JSONObject payload = new JSONObject();
            payload.put("content", repost.getText().isBlank() ? "[Original message unavailable]" : repost.getText());
            payload.put("username", repost.getChatName());
            payload.put("avatar_url", repost.getChatIcon());

            HttpEntity<String> request = new HttpEntity<>(payload.toString(), headers);
            restTemplate.postForEntity(webhook, request, String.class);
    }
}}
